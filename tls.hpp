// 可选 TLS 服务端（OpenSSL）：设置 TLS_CERT / TLS_KEY 后，监听端口改为 HTTPS。
//   - 证书/私钥在启动时加载；握手在连接建立后异步完成（非阻塞，复用 io_context 操作槽）；
//   - TLS 下 sendfile 不适用，静态资源回退为普通写（调用方按 m_tls 判断）。
#pragma once

#include <cstdio>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

struct tls_context {
    SSL_CTX *ctx = nullptr;

    [[nodiscard]] bool enabled() const {
        return ctx != nullptr;
    }

    bool init(std::string const &cert_file, std::string const &key_file) {
        OPENSSL_init_ssl(0, nullptr);
        ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) {
            std::fprintf(stderr, "[tls] SSL_CTX_new failed\n");
            return false;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
        if (SSL_CTX_use_certificate_chain_file(ctx, cert_file.c_str()) != 1) {
            std::fprintf(stderr, "[tls] load cert '%s' failed: %s\n", cert_file.c_str(),
                         ERR_error_string(ERR_get_error(), nullptr));
            SSL_CTX_free(ctx);
            ctx = nullptr;
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::fprintf(stderr, "[tls] load key '%s' failed: %s\n", key_file.c_str(),
                         ERR_error_string(ERR_get_error(), nullptr));
            SSL_CTX_free(ctx);
            ctx = nullptr;
            return false;
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            std::fprintf(stderr, "[tls] private key does not match certificate\n");
            SSL_CTX_free(ctx);
            ctx = nullptr;
            return false;
        }
        return true;
    }

    SSL *new_ssl(int fd) {
        if (!ctx) {
            return nullptr;
        }
        SSL *ssl = SSL_new(ctx);
        if (!ssl) {
            return nullptr;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_accept_state(ssl);
        return ssl;
    }

    ~tls_context() {
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
    }
};

inline tls_context &tls() {
    static tls_context instance;
    return instance;
}
