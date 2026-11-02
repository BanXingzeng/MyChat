#-------------------------------------------------
# OpenSSL 4.1.0 (Win32 / mingw730_32) 依赖
# 供客户端、服务端共用：消息端到端加密
#-------------------------------------------------

OPENSSL_DIR = $$PWD

INCLUDEPATH += $$OPENSSL_DIR/include
DEPENDPATH  += $$OPENSSL_DIR/include

# libcrypto.dll.a 是用官方 .def 重新生成的 32 位导入库
# （发行包自带的 lib/MinGW/x86 里装的是 x86-64 目标文件，和 32 位 DLL 位数不一致）
LIBS += -L$$OPENSSL_DIR/lib -lcrypto

# 构建完成后把运行库拷到可执行文件目录，保证双击 / 调试都能直接跑
win32 {
    SSL_DLL  = $$shell_path($$OPENSSL_DIR/bin/libcrypto-4.dll)
    SSL_ROOT = $$shell_path($$OUT_PWD)

    # mingw 的 make 走 sh，QMAKE_COPY 默认是 cmd 的 "copy /y"，
    # 直接拼进 POST_LINK 会报 "copy: command not found"（被 - 忽略掉，DLL 就悄悄不更新了）
    win32-g++: SSL_COPY = cp -f
    else:      SSL_COPY = $$QMAKE_COPY

    QMAKE_POST_LINK += -$$SSL_COPY $$shell_quote($$SSL_DLL) $$shell_quote($$SSL_ROOT) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$$SSL_COPY $$shell_quote($$SSL_DLL) $$shell_quote($$SSL_ROOT/debug) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += -$$SSL_COPY $$shell_quote($$SSL_DLL) $$shell_quote($$SSL_ROOT/release)
}
