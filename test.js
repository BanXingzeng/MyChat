#!/usr/bin/env node
'use strict';
/*
 * ============================================================================
 *  MyChat 服务端并发压测脚本
 *
 *  按协议 v2/v3 完整实现客户端：长度前缀分帧 -> CH/SH 握手（验签 + X25519 +
 *  HKDF）-> CF/SF 双向确认 -> 挑战应答登录 -> AEAD 加密帧 + 心跳。
 *  只有真走完这套流程，连上的才算"有效在线用户"，压测结果才有意义。
 *
 *  （原来那份 k6 的 http.get 模板测的是 HTTP 接口，与本服务端的自定义 TCP
 *    协议不兼容，已整体替换。）
 *
 *  两种口径，别混：
 *    · 稳态在线容量 —— 匀速爬坡（--ramp），测"能同时挂多少人且不掉线"
 *    · 瞬时并发冲击 —— 同一刻全部发起（--burst），测"一波涌入能接住多少、
 *      多久全部就绪"。两者瓶颈完全不同：前者看事件循环和内存，后者看
 *      accept 速率和握手吞吐（服务端握手是单线程串行的，约 5 ms/个）。
 *
 *  用法：
 *    node test.js                       # 默认 100 并发
 *    node test.js --clients 300         # 300 并发
 *    node test.js --clients 500 --ramp 50 --hold 60   # 爬坡：稳态在线
 *    node test.js --clients 1000 --burst --hold 30    # 瞬时并发：一波涌入
 *    node test.js --seed 500            # 只造 500 个测试账号
 *    node test.js --clean               # 清掉测试账号
 *    node test.js --no-login            # 只测握手+连接容量，跳过登录
 *
 *  重要：每个连接必须用不同账号。服务端对同一账号重复登录会回 LoginRepeat，
 *        所以脚本默认自动造一批 lt_1..lt_N 测试账号（直接写 SQLite）。
 *
 *  建议：造号时先停服务端、造完再启动，避免 SQLite 写锁争用。
 *        并发超过 150 时会自动带上 UV_THREADPOOL_SIZE=32 重启自身，
 *        不用手工设环境变量（登录的 PBKDF2 走线程池，默认只有 4 个线程）。
 * ============================================================================
 */

const net = require('net');
const crypto = require('crypto');
const path = require('path');
const fs = require('fs');

// ---------------------------------------------------------------- 协议常量
const E_MSG = {
    Login: 0x11,
    LoginSuccess: 0x03,
    LoginPasswdError: 0x04,
    LoginRepeat: 0x05,
    HeartBeat: 0x6A,
    HeartBeatAck: 0x7A,
};
const AAD_LABEL = Buffer.from('MC2/REC/v2', 'ascii');   // 10 字节
const FIN_LABEL = 'MC2/fin';
const AUTH_LABEL = Buffer.from('MC2/AUTH', 'ascii');    // 8 字节
const X25519_SPKI_PREFIX = Buffer.from('302a300506032b656e032100', 'hex');

// ---------------------------------------------------------------- 参数
function parseArgs(argv) {
    const o = {
        host: '127.0.0.1',
        port: 60101,     // 服务端实际监听端口（默认配置 60101）
        clients: 100,
        ramp: 50,              // 每秒建立多少个连接；<=0 表示瞬时并发（所有连接同一刻发起）
        hold: 20,              // 登录成功后保持多少秒
        hb: 5,                 // 心跳间隔（秒）
        prefix: 'lt_',
        passwd: 'test123456',
        iter: 10000,           // 测试账号的 PBKDF2 迭代数（真实账号更高，这里避免客户端成瓶颈）
        db: '',
        seed: 0,
        clean: false,
        login: true,
        timeout: 15000,
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const next = () => argv[++i];
        switch (a) {
            case '--host': o.host = next(); break;
            case '--port': o.port = parseInt(next(), 10); break;
            case '--clients': o.clients = parseInt(next(), 10); break;
            case '--ramp': o.ramp = parseFloat(next()); break;
            case '--burst': o.ramp = 0; break;
            case '--prefix': o.prefix = next(); break;
            case '--hold': o.hold = parseFloat(next()); break;
            case '--hb': o.hb = parseFloat(next()); break;
            case '--db': o.db = next(); break;
            case '--seed': o.seed = parseInt(next(), 10); break;
            case '--clean': o.clean = true; break;
            case '--no-login': o.login = false; break;
            case '--help':
            case '-h':
                printHelp();
                process.exit(0);
            default:
                if (a.startsWith('--')) {
                    console.error('未知参数:', a);
                    process.exit(2);
                }
        }
    }
    return o;
}

function printHelp() {
    console.log(`
用法: node test.js [选项]

  --host <ip>       服务端地址（默认 127.0.0.1）
  --port <n>        服务端端口（默认 60101）
  --clients <n>     并发连接数（默认 100）
  --ramp <n>        每秒建立连接数（默认 50；0 = 瞬时并发，全部同一刻发起）
  --burst           等价于 --ramp 0，测瞬时并发建连冲击
  --prefix <s>      测试账号前缀（默认 lt_，多进程分片时用来错开账号段）
  --hold <s>        登录后保持秒数（默认 20）
  --hb <s>          心跳间隔秒数（默认 5）
  --db <path>       服务端 server.db 路径（默认自动查找）
  --seed <n>        只造 n 个测试账号后退出
  --clean           删除所有测试账号后退出
  --no-login        只测 TCP + 握手，跳过登录

示例:
  node test.js --clients 300 --ramp 60 --hold 60      # 稳态在线容量
  node test.js --clients 1000 --burst --hold 30       # 瞬时并发建连
  UV_THREADPOOL_SIZE=32 node test.js --clients 500
`);
}

// ---------------------------------------------------------------- 基础工具
const b64 = (b) => Buffer.from(b).toString('base64');
const unb64 = (s) => Buffer.from(String(s || ''), 'base64');

function u64be(n) {
    const b = Buffer.alloc(8);
    b.writeBigUInt64BE(BigInt(n));
    return b;
}

function sha256(b) {
    return crypto.createHash('sha256').update(b).digest();
}

function hmac(key, data) {
    return crypto.createHmac('sha256', key).update(data).digest();
}

/// RFC 5869 HKDF（extract + expand 一步到位，与两端 CryptoUtil 等价）
function hkdf(ikm, salt, info, len) {
    return Buffer.from(crypto.hkdfSync('sha256', ikm, salt, info, len));
}

function pbkdf2Async(passwd, salt, iter, len) {
    return new Promise((resolve, reject) => {
        crypto.pbkdf2(passwd, salt, iter, len, 'sha256',
            (err, key) => (err ? reject(err) : resolve(key)));
    });
}

function verifyAsync(pem, data, sig) {
    return new Promise((resolve) => {
        try {
            const key = crypto.createPublicKey(pem);
            crypto.verify('sha256', data, {
                key,
                padding: crypto.constants.RSA_PKCS1_PSS_PADDING,
                saltLength: crypto.constants.RSA_PSS_SALTLEN_DIGEST,  // 与服务端一致
            }, sig, (err, ok) => resolve(!err && ok === true));
        } catch (e) {
            resolve(false);
        }
    });
}

function spkiDigest(pem) {
    try {
        const der = crypto.createPublicKey(pem).export({ type: 'spki', format: 'der' });
        return sha256(der);
    } catch (e) {
        return Buffer.alloc(0);
    }
}

function pct(arr, p) {
    if (!arr.length) return 0;
    const s = [...arr].sort((a, b) => a - b);
    const i = Math.min(s.length - 1, Math.max(0, Math.ceil((p / 100) * s.length) - 1));
    return s[i];
}

// ---------------------------------------------------------------- 数据库：造号 / 清理
function findDb(explicit) {
    if (explicit) return explicit;
    const root = __dirname;
    const guess = path.join(root, 'Server', 'build', 'Q5_14_2_MinGW_32_bin_Release',
        'release', 'Data', 'Database', 'server.db');
    if (fs.existsSync(guess)) return guess;
    const base = path.join(root, 'Server', 'build');
    if (fs.existsSync(base)) {
        for (const d of fs.readdirSync(base)) {
            const p = path.join(base, d, 'release', 'Data', 'Database', 'server.db');
            if (fs.existsSync(p)) return p;
        }
    }
    return '';
}

function makeHash(plain, iter) {
    const salt = crypto.randomBytes(16);
    const digest = crypto.pbkdf2Sync(Buffer.from(plain, 'utf8'), salt, iter, 32, 'sha256');
    return `pbkdf2-sha256$${iter}$${b64(salt)}$${b64(digest)}`;
}

/// 造测试账号。pub_key 留空 -> 登录只验口令，不需要身份私钥签名
function seedAccounts(dbPath, n, prefix, passwd, iter) {
    const { DatabaseSync } = require('node:sqlite');
    const db = new DatabaseSync(dbPath);
    try {
        db.exec('PRAGMA busy_timeout=8000');
        const like = prefix.replace(/_/g, '\\_') + '%';
        db.prepare("DELETE FROM USERINFO WHERE name LIKE ? ESCAPE '\\'").run(like);

        const ins = db.prepare(
            'INSERT INTO USERINFO (name, passwd, head, phone, work_group, email, pub_key, status, lasttime) ' +
            "VALUES (?, ?, '', '', '', '', '', 6, datetime('now','localtime'))");

        db.exec('BEGIN');
        let made = 0;
        for (let i = 1; i <= n; i++) {
            try {
                ins.run(prefix + i, makeHash(passwd, iter));
                made++;
            } catch (e) {
                // 用户名冲突等，跳过
            }
        }
        db.exec('COMMIT');
        return made;
    } finally {
        db.close();
    }
}

function cleanAccounts(dbPath, prefix) {
    const { DatabaseSync } = require('node:sqlite');
    const db = new DatabaseSync(dbPath);
    try {
        db.exec('PRAGMA busy_timeout=8000');
        const like = prefix.replace(/_/g, '\\_') + '%';
        const row = db.prepare("SELECT count(*) AS c FROM USERINFO WHERE name LIKE ? ESCAPE '\\'").get(like);
        db.prepare("DELETE FROM USERINFO WHERE name LIKE ? ESCAPE '\\'").run(like);
        return row ? Number(row.c) : 0;
    } finally {
        db.close();
    }
}

// ---------------------------------------------------------------- 连接
class BenchClient {
    constructor(opt, index) {
        this.opt = opt;
        this.index = index;
        this.name = opt.prefix + (index + 1);
        this.sock = null;
        this.buf = Buffer.alloc(0);

        this.secure = false;
        this.kc2s = null;
        this.ks2c = null;
        this.sid = Buffer.alloc(0);
        this.sendSeq = 1;
        this.recvSeq = 1;

        this.queue = [];          // 已解出的明文帧
        this.waiters = [];
        this.closed = false;
        this.closeReason = '';
        this.hbSent = [];         // 待确认的心跳发出时刻
        this.hbRtt = [];
        this.inbox = 0;           // 登录后收到的推送数量

        this.tHandshake = 0;
        this.tConnect = 0;        // TCP 三次握手耗时（从发起 connect 到可写）
        this.atConnect = 0;       // 建连完成的绝对时刻（相对压测起点），用于算建连窗口
        this.connectOk = false;
        this.tLogin = 0;
        this.uid = 0;
        this.error = '';
    }

    // ---- 网络
    connect() {
        return new Promise((resolve, reject) => {
            const tc = performance.now();
            const s = net.createConnection({ host: this.opt.host, port: this.opt.port });
            this.sock = s;
            s.setNoDelay(true);
            const timer = setTimeout(() => {
                s.destroy();
                reject(new Error('TCP 连接超时'));
            }, this.opt.timeout);

            s.once('connect', () => {
                clearTimeout(timer);
                this.tConnect = performance.now() - tc;
                this.atConnect = performance.now();   // 绝对时刻，事后减去压测起点
                this.connectOk = true;
                resolve();
            });
            s.once('error', (e) => {
                clearTimeout(timer);
                // 保留系统错误码：ECONNREFUSED 说明 accept 队列/端口拒了，
                // ETIMEDOUT 说明 SYN 被丢（Windows 上 backlog 打满就是这种表现）
                reject(new Error(e.code ? `${e.code} ${e.message}` : e.message));
            });
            s.on('data', (chunk) => this._onData(chunk));
            s.on('close', () => {
                this.closed = true;
                this._flushWaiters(new Error(this.closeReason || '对端关闭连接'));
            });
        });
    }

    send(obj) {
        if (this.closed || !this.sock || this.sock.destroyed) return false;
        const payload = Buffer.from(JSON.stringify(obj), 'utf8');
        const head = Buffer.alloc(4);
        head.writeUInt32BE(payload.length, 0);
        this.sock.write(Buffer.concat([head, payload]));
        return true;
    }

    _onData(chunk) {
        this.buf = Buffer.concat([this.buf, chunk]);
        for (;;) {
            if (this.buf.length < 4) return;
            const n = this.buf.readUInt32BE(0);
            if (n > 8 * 1024 * 1024) {          // 与服务端同款上限
                this.closeReason = '帧长度异常';
                this.sock.destroy();
                return;
            }
            if (this.buf.length < 4 + n) return;
            const payload = this.buf.subarray(4, 4 + n);
            this.buf = this.buf.subarray(4 + n);
            this._onFrame(payload);
        }
    }

    _onFrame(payload) {
        let obj;
        try {
            obj = JSON.parse(payload.toString('utf8'));
        } catch (e) {
            return;
        }

        if (!this.secure) {
            this._push(obj);                    // 握手阶段是明文 JSON
            return;
        }

        // 加密通道：{c: base64(ct||tag)}
        const raw = unb64(obj.c);
        if (raw.length < 16) return;
        const ct = raw.subarray(0, raw.length - 16);
        const tag = raw.subarray(raw.length - 16);
        const seq = this.recvSeq++;

        const aad = Buffer.concat([AAD_LABEL, Buffer.from([2]), u64be(seq)]);
        const nonce = Buffer.concat([Buffer.alloc(4), u64be(seq)]);
        let plain;
        try {
            const d = crypto.createDecipheriv('aes-256-gcm', this.ks2c, nonce);
            d.setAAD(aad, { plaintextLength: ct.length });
            d.setAuthTag(tag);
            plain = Buffer.concat([d.update(ct), d.final()]);
        } catch (e) {
            this.closeReason = 'AEAD 解密失败';
            this.sock.destroy();
            return;
        }

        let inner;
        try {
            inner = JSON.parse(plain.toString('utf8'));
        } catch (e) {
            return;
        }

        if (inner.type === E_MSG.HeartBeatAck && this.hbSent.length) {
            const t = this.hbSent.shift();
            this.hbRtt.push(performance.now() - t);
        }
        this.inbox++;
        this._push(inner);
    }

    _push(obj) {
        const idx = this.waiters.findIndex((w) => w.pred(obj));
        if (idx >= 0) {
            const w = this.waiters.splice(idx, 1)[0];
            clearTimeout(w.timer);
            w.resolve(obj);
        } else {
            this.queue.push(obj);
            if (this.queue.length > 200) this.queue.shift();
        }
    }

    _flushWaiters(err) {
        for (const w of this.waiters.splice(0)) {
            clearTimeout(w.timer);
            w.reject(err);
        }
    }

    waitFor(pred, timeoutMs = 15000) {
        const hit = this.queue.findIndex(pred);
        if (hit >= 0) return Promise.resolve(this.queue.splice(hit, 1)[0]);
        return new Promise((resolve, reject) => {
            const w = { pred, resolve, reject, timer: null };
            w.timer = setTimeout(() => {
                const i = this.waiters.indexOf(w);
                if (i >= 0) this.waiters.splice(i, 1);
                reject(new Error('等待响应超时'));
            }, timeoutMs);
            this.waiters.push(w);
        });
    }

    // ---- 加密帧
    seal(obj) {
        const plain = Buffer.from(JSON.stringify(obj), 'utf8');
        const seq = this.sendSeq++;
        const aad = Buffer.concat([AAD_LABEL, Buffer.from([1]), u64be(seq)]);
        const nonce = Buffer.concat([Buffer.alloc(4), u64be(seq)]);
        const c = crypto.createCipheriv('aes-256-gcm', this.kc2s, nonce);
        c.setAAD(aad, { plaintextLength: plain.length });
        const ct = Buffer.concat([c.update(plain), c.final()]);
        return { c: b64(Buffer.concat([ct, c.getAuthTag()])) };
    }

    // ---- 握手
    async handshake() {
        const { publicKey, privateKey } = crypto.generateKeyPairSync('x25519');
        const ephC = publicKey.export({ type: 'spki', format: 'der' }).subarray(-32);
        const nonceC = crypto.randomBytes(32);
        const cid = crypto.randomBytes(16);

        this.send({
            hs: 'CH',
            ver: 2,
            nonce_c: b64(nonceC),
            eph_c: b64(ephC),
            cid: b64(cid),
            algs: ['X25519', 'AES-256-GCM', 'RSA-PSS-SHA256', 'HKDF-SHA256'],
        });

        let sh;
        try {
            sh = await this.waitFor((o) => o.hs === 'SH', this.opt.timeout);
        } catch (e) {
            // TCP 通了却收不到 SH：几乎都是对端不认这套协议（旧版服务端），
            // 或该端口上根本不是本服务端。单独给个可辨识的错误，别混进"超时"里。
            throw new Error('服务端未响应握手：TCP 已连接，但没有 SH 回应');
        }
        const nonceS = unb64(sh.nonce_s);
        const ephS = unb64(sh.eph_s);
        this.sid = unb64(sh.sid);
        if (ephS.length !== 32) throw new Error('服务端临时公钥长度异常');

        const cert = sh.cert || {};
        const serverPub = String(cert.pub || '');
        const fp = spkiDigest(serverPub);
        if (fp.length !== 32) throw new Error('服务器公钥解析失败');
        if (!fp.equals(unb64(cert.fp))) throw new Error('cert.fp 与公钥不符');

        const transcript = sha256(Buffer.concat([
            Buffer.from('MC2/SH', 'ascii'), Buffer.from([2]),
            nonceC, nonceS, ephC, ephS, this.sid, fp,
        ]));

        const sigOk = await verifyAsync(serverPub, transcript, unb64(sh.sig));
        if (!sigOk) throw new Error('服务器签名校验失败（SH 被篡改或公钥不对）');

        const peer = crypto.createPublicKey({
            key: Buffer.concat([X25519_SPKI_PREFIX, ephS]),
            format: 'der', type: 'spki',
        });
        const z = crypto.diffieHellman({ privateKey, publicKey: peer });

        const salt = sha256(Buffer.concat([nonceC, nonceS]));
        this.kc2s = hkdf(z, salt, Buffer.concat([Buffer.from('MC2/c2s'), this.sid]), 32);
        this.ks2c = hkdf(z, salt, Buffer.concat([Buffer.from('MC2/s2c'), this.sid]), 32);

        this.send({
            hs: 'CF',
            fin: b64(hmac(this.kc2s, Buffer.concat([Buffer.from(FIN_LABEL), transcript]))),
        });

        const sf = await this.waitFor((o) => o.hs === 'SF', this.opt.timeout);
        const want = hmac(this.ks2c, Buffer.concat([Buffer.from(FIN_LABEL), transcript]));
        if (!want.equals(unb64(sf.fin))) throw new Error('SF 校验失败（双方密钥不一致）');

        this.secure = true;
        this.queue.length = 0;                  // 握手期间的残留帧作废
    }

    // ---- 登录（挑战-应答）
    async login() {
        this.send(this.seal({
            type: E_MSG.Login,
            data: { name: this.name, step: 'init' },
        }));

        const chal = await this.waitFor(
            (o) => o.data && o.data.step === 'challenge', this.opt.timeout);

        const salt = unb64(chal.data.salt);
        const nonce = unb64(chal.data.nonce);
        const iter = parseInt(chal.data.iter, 10) || 10000;

        const verifier = await pbkdf2Async(Buffer.from(this.opt.passwd, 'utf8'), salt, iter, 32);

        const material = Buffer.concat([
            AUTH_LABEL, this.sid, nonce, Buffer.from(this.name, 'utf8'),
        ]);

        this.send(this.seal({
            type: E_MSG.Login,
            data: { name: this.name, step: 'proof', proof: b64(hmac(verifier, material)) },
        }));

        const res = await this.waitFor(
            (o) => o.type === E_MSG.LoginSuccess
                || o.type === E_MSG.LoginPasswdError
                || o.type === E_MSG.LoginRepeat,
            this.opt.timeout);

        if (res.type === E_MSG.LoginSuccess) {
            this.uid = parseInt((res.data || {}).id, 10) || 0;
            return;
        }
        const why = (res.data || {}).msg
            || (res.type === E_MSG.LoginRepeat ? '同一账号已在别处登录' : '口令错误');
        throw new Error('登录失败: ' + why);
    }

    // ---- 心跳
    beat() {
        if (this.closed) return;
        this.hbSent.push(performance.now());
        this.send(this.seal({ type: E_MSG.HeartBeat, data: {} }));
    }

    close() {
        if (this.sock && !this.sock.destroyed) this.sock.destroy();
    }
}

// ---------------------------------------------------------------- 预检
/*
 * 先用一条连接走完握手就断开，确认对面讲的是同一套协议。
 * 这一步能在 2 秒内把"旧版服务端/端口被占"和"服务端太慢"区分开，
 * 否则 100 个连接要各自白等 15 秒，最后只看到一句"等待响应超时"。
 */
async function preflight(opt) {
    const probe = new BenchClient({ ...opt, timeout: 4000 }, 0);
    probe.name = opt.prefix + 'probe';
    try {
        await probe.connect();
    } catch (e) {
        return { tcp: false, err: (e && e.message) || String(e) };
    }
    try {
        await probe.handshake();
        return { tcp: true, hs: true };
    } catch (e) {
        return { tcp: true, hs: false, err: (e && e.message) || String(e) };
    } finally {
        probe.close();
    }
}

function printPreflightFail(pf, opt, dbPath) {
    if (!pf.tcp) {
        console.error(`\n预检失败：连不上 ${opt.host}:${opt.port}  （${pf.err}）`);
        console.error('  1) 服务端没启动 —— 先运行 Server.exe');
        console.error('  2) 端口不是 ' + opt.port + ' —— 用 --port 指定实际端口');
        console.error('  3) 防火墙拦了 —— 本机测试放到同一台机器上跑');
        return;
    }
    console.error(`\n预检失败：${pf.err}`);
    console.error('  TCP 三次握手是通的，说明端口上确实有程序在监听，但对方不认本协议。');
    console.error('  最可能的原因是跑错了 Server.exe：');
    console.error('    · 旧版（如 bundled\\Server\\Server.exe，2026-11-02 及更早的构建）');
    console.error('      还没有握手协议，收到 CH 帧会直接忽略，于是什么都不回。');
    console.error('    · 正确的新版在：');
    console.error('      Server\\build\\Q5_14_2_MinGW_32_bin_Release\\release\\Server.exe');
    console.error('  怎么确认：看服务端窗口的标题/日志里有没有握手相关输出；');
    console.error('            或比对文件时间，支持握手的构建是 2026-11-03 之后的。');
    if (dbPath) {
        console.error(`\n  另注意：本次造号写入的是\n    ${dbPath}`);
        console.error('  若实际运行的服务端读取的是另一个 server.db（比如 bundled 目录下的），');
        console.error('  即使握手通了，登录也会报"口令错误"，届时用 --db 指到服务端真正用的库。');
    }
}

// ---------------------------------------------------------------- 主流程
async function main() {
    const opt = parseArgs(process.argv.slice(2));

    /*
     * libuv 线程池大小只在 node 启动那一刻读一次，脚本里改 process.env 已经晚了。
     * 并发超过 150 时 PBKDF2 会挤在默认 4 个线程上排队，测出来的是客户端瓶颈。
     * 所以这里带着环境变量把自己重启一次 —— cmd / PowerShell / bash 都能用，
     * 也就不用记各 shell 设环境变量的语法（cmd 的 VAR=值 命令 是无效的）。
     */
    if (opt.clients > 150 && opt.seed === 0 && !opt.clean
        && !process.env.UV_THREADPOOL_SIZE && !process.env.MYCHAT_BENCH_REEXEC) {
        const { spawn } = require('child_process');
        console.log(`线程池    并发 ${opt.clients} > 150，自动以 UV_THREADPOOL_SIZE=32 重启自身\n`);
        const child = spawn(process.execPath, [__filename, ...process.argv.slice(2)], {
            stdio: 'inherit',
            env: { ...process.env, UV_THREADPOOL_SIZE: '32', MYCHAT_BENCH_REEXEC: '1' },
        });
        child.on('exit', (code) => process.exit(code === null ? 1 : code));
        return;
    }

    const dbPath = findDb(opt.db);

    if (opt.seed > 0 || opt.clean) {
        if (!dbPath) {
            console.error('找不到 server.db，请用 --db 指定路径');
            process.exit(1);
        }
        console.log('数据库:', dbPath);
        if (opt.clean) {
            const n = cleanAccounts(dbPath, opt.prefix);
            console.log(`已删除 ${n} 个测试账号（${opt.prefix}*）`);
        }
        if (opt.seed > 0) {
            const n = seedAccounts(dbPath, opt.seed, opt.prefix, opt.passwd, opt.iter);
            console.log(`已创建 ${n} 个测试账号：${opt.prefix}1 .. ${opt.prefix}${opt.seed}`);
            console.log(`口令统一为 ${opt.passwd}`);
        }
        return;
    }

    console.log('=== MyChat 服务端并发压测 ===');
    console.log(`目标      ${opt.host}:${opt.port}`);
    console.log(`连接数    ${opt.clients}   ${opt.ramp > 0 ? `爬坡 ${opt.ramp}/s` : '瞬时并发'}   ` +
        `保持 ${opt.hold}s   心跳 ${opt.hb}s`);
    console.log(`登录      ${opt.login ? '启用（挑战-应答）' : '跳过'}`);
    if (process.env.UV_THREADPOOL_SIZE) {
        console.log(`线程池    UV_THREADPOOL_SIZE=${process.env.UV_THREADPOOL_SIZE}`);
    }

    // ---- 阶段 0：预检。失败就别造号、更别连 100 次白等
    console.log('\n[0/3] 预检：确认服务端讲的是同一套协议 ...');
    const pf = await preflight(opt);
    if (!pf.hs) {
        printPreflightFail(pf, opt, dbPath);
        process.exit(1);
    }
    console.log('      握手正常，服务端支持本协议');

    if (opt.login && dbPath) {
        const n = seedAccounts(dbPath, opt.clients, opt.prefix, opt.passwd, opt.iter);
        console.log(`账号      ${dbPath}`);
        console.log(`          已就绪 ${n} 个（${opt.prefix}1 .. ${opt.prefix}${opt.clients}）\n`);
    } else if (opt.login) {
        console.log('账号      未找到服务器数据库，假定测试账号已存在\n');
    } else {
        console.log('');
    }

    const clients = [];
    const okHandshake = [];
    const okLogin = [];
    const hsTimes = [];
    const loginTimes = [];
    const failHandshake = new Map();
    const failLogin = new Map();
    const logFail = (map, msg) => map.set(msg, (map.get(msg) || 0) + 1);

    // ---- 阶段 1：建连 + 握手 + 登录
    console.log('[1/3] 建立连接并完成握手/登录 ...');
    const t0 = performance.now();

    const oneClient = async (i) => {
        const c = new BenchClient(opt, i);
        clients.push(c);
        const tg = performance.now();
        try {
            await c.connect();
            await c.handshake();
            c.tHandshake = performance.now() - tg;
            hsTimes.push(c.tHandshake);
            okHandshake.push(c);

            if (opt.login) {
                const tl = performance.now();
                await c.login();
                c.tLogin = performance.now() - tl;
                loginTimes.push(c.tLogin);
                okLogin.push(c);
            }
            return true;
        } catch (e) {
            const msg = (e && e.message) ? e.message : String(e);
            if (c.tHandshake > 0) logFail(failLogin, msg);
            else logFail(failHandshake, msg);
            c.error = msg;
            c.close();
            return false;
        }
    };

    /*
     * 心跳从"连上并握手完成"那一刻就开始，而不是等到保持阶段。
     * 否则爬坡慢的时候（2000 并发 × 30/s = 66 秒），最早连上的那批会在 60 秒后
     * 撞上服务端的 sweepIdleClients（超 60 秒无数据即判下线），
     * 明明活着却被踢掉——2000 并发时正好踢了 271 个。
     */
    const hbTimer = setInterval(() => {
        // --no-login 时服务端把这条连接当"未认证"，收到心跳会直接 abort
        // （设计如此：未认证连接只允许 Login/Register）。所以这种模式不发心跳，
        // 否则会看到"几百个连接 3 秒后集体掉线"的假故障。
        if (!opt.login) return;
        for (const c of clients) if (c.secure && !c.closed) c.beat();
    }, opt.hb * 1000);

    const tasks = [];
    const burst = !(opt.ramp > 0);
    if (burst) {
        console.log(`      模式          瞬时并发：${opt.clients} 个 SYN 在同一轮事件循环里全部发出`);
    }
    for (let i = 0; i < opt.clients; i++) {
        if (burst) {
            // 不排队、不等定时器，直接同步发起 connect —— 这才是"同一刻"
            tasks.push(oneClient(i));
        } else {
            // 匀速放行，测的是稳态在线容量，不是并发冲击
            const delay = (i / opt.ramp) * 1000;
            tasks.push(new Promise((resolve) => {
                setTimeout(() => { oneClient(i).then(resolve); }, delay);
            }));
        }
    }
    await Promise.all(tasks);

    const tConnect = performance.now() - t0;
    const online = opt.login ? okLogin.length : okHandshake.length;

    // ---- 建连（三次握手）单独统计：瞬时并发下这才是被冲击的指标
    const connOk = clients.filter((c) => c.connectOk);
    const connTimes = connOk.map((c) => c.tConnect);
    const connFail = opt.clients - connOk.length;
    if (connTimes.length) {
        const span = (Math.max(...connOk.map((c) => c.atConnect)) - t0) / 1000;
        const rate = span > 0.001 ? (connTimes.length / span).toFixed(0) : '—';
        console.log(`      建成连接      ${connOk.length}/${opt.clients}` +
            (connFail > 0 ? `   （失败 ${connFail}）` : ''));
        console.log(`      建连延迟      p50 ${pct(connTimes, 50).toFixed(0)}ms  ` +
            `p95 ${pct(connTimes, 95).toFixed(0)}ms  ` +
            `p99 ${pct(connTimes, 99).toFixed(0)}ms  max ${Math.max(...connTimes).toFixed(0)}ms`);
        console.log(`      建连窗口      全部连接在 ${span.toFixed(2)}s 内建立  (约 ${rate} 个/秒被 accept)`);
    }
    console.log(`      连接+握手成功  ${okHandshake.length}/${opt.clients}`);
    console.log(`      总耗时        ${(tConnect / 1000).toFixed(1)} s`);
    if (hsTimes.length) {
        console.log(`      握手延迟      p50 ${pct(hsTimes, 50).toFixed(0)}ms  ` +
            `p95 ${pct(hsTimes, 95).toFixed(0)}ms  max ${Math.max(...hsTimes).toFixed(0)}ms`);
    }
    if (opt.login) {
        console.log(`      登录成功      ${okLogin.length}/${opt.clients}`);
        if (loginTimes.length) {
            console.log(`      登录延迟      p50 ${pct(loginTimes, 50).toFixed(0)}ms  ` +
                `p95 ${pct(loginTimes, 95).toFixed(0)}ms  max ${Math.max(...loginTimes).toFixed(0)}ms`);
        }
    }
    for (const [msg, n] of failHandshake) console.log(`      [握手失败] ${n} 个  ${msg}`);
    for (const [msg, n] of failLogin) console.log(`      [登录失败] ${n} 个  ${msg}`);

    if (online === 0) {
        console.log('\n没有任何连接成功，压测终止。检查服务端是否在运行、账号是否存在。');
        process.exit(1);
    }

    // ---- 阶段 2：保持连接 + 心跳
    console.log(`\n[2/3] 保持 ${opt.hold}s，每 ${opt.hb}s 一次心跳 ...`);
    const live = clients.filter((c) => !c.closed);
    const peakOnline = live.length;

    const samples = [];
    const t1 = performance.now();
    await new Promise((resolve) => {
        const sampler = setInterval(() => {
            const now = performance.now();
            samples.push({ t: (now - t1) / 1000, alive: live.filter((c) => !c.closed).length });
            if (now - t1 >= opt.hold * 1000) {
                clearInterval(sampler);
                resolve();
            }
        }, 1000);
    });
    clearInterval(hbTimer);

    const aliveEnd = live.filter((c) => !c.closed).length;
    const dropped = live.filter((c) => c.closed);
    const rtts = [];
    for (const c of live) rtts.push(...c.hbRtt);

    console.log(`      起始在线      ${peakOnline}`);
    console.log(`      结束在线      ${aliveEnd}`);
    console.log(`      掉线          ${dropped.length}`);
    if (rtts.length) {
        console.log(`      心跳 RTT      p50 ${pct(rtts, 50).toFixed(1)}ms  ` +
            `p95 ${pct(rtts, 95).toFixed(1)}ms  max ${Math.max(...rtts).toFixed(1)}ms  ` +
            `(${rtts.length} 次)`);
    }
    for (const c of dropped.slice(0, 5)) {
        console.log(`      [掉线] ${c.name}  ${c.closeReason || c.error || '未知原因'}`);
    }

    // ---- 阶段 3：结论
    console.log('\n[3/3] 结论');
    const successRate = (online / opt.clients) * 100;
    console.log(`      有效在线      ${online} / ${opt.clients}  (${successRate.toFixed(1)}%)`);
    console.log(`      稳态在线      ${aliveEnd}  持续 ${opt.hold}s`);

    if (dropped.length === 0 && successRate >= 99) {
        console.log(`\n      本次 ${opt.clients} 并发全部稳定，可继续加大 --clients 找上限。`);
    } else if (successRate < 90 || dropped.length > 0) {
        console.log('\n      出现失败或掉线，服务端可能已到瓶颈，或账号/端口配置有误。');
    }

    if (samples.length > 1) {
        const step = Math.max(1, Math.floor(samples.length / 12));
        const curve = samples.filter((_, i) => i % step === 0)
            .map((s) => `${s.t.toFixed(0)}s:${s.alive}`).join('  ');
        console.log(`\n      在线曲线  ${curve}`);
    }

    for (const c of clients) c.close();
    await new Promise((r) => setTimeout(r, 300));
    process.exit(0);
}

main().catch((e) => {
    console.error('压测异常终止:', e);
    process.exit(1);
});
