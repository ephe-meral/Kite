#include "SecureSocket.hpp"
#include <stdio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sstream>
#include <iostream>

#include "Timer.hpp"

using namespace Kite;

//#define debugprintf(...) fprintf(stderr, __VA_ARGS__)
#define debugprintf(...)

//TODO: not thread safe
static std::map<SSL  *, SecureSocket *> rebind_map;

class Kite::SecureSocketPrivate : public Kite::Evented {
public:
    SSL_CTX *ssl_ctx;
    BIO     *bio;
    SSL     *ssl; //do note delete, is a ref from bio
    SecureSocket::SocketState state;
    std::string errorMessage;

    bool useTls;
    bool  hasTimeout;
    Timer connectionTimout;

    SecureSocket *p;
    void d_connect();

    SecureSocketPrivate(std::weak_ptr<Kite::EventLoop> ev)
        : Kite::Evented(ev)
        , connectionTimout(ev)
    {
        KITE_TIMER_DEBUG_NAME(&connectionTimout, "Kite::SecureSocketPrivate::connectionTimout");
    }
    virtual void onActivated (int)
    {
        if (state == Kite::SecureSocket::Connecting) {
            d_connect();
        } else if (state == Kite::SecureSocket::Connected) {
            p->onActivated(0);
        }
    }
    friend class Kite::SecureSocket;
};

void apps_ssl_info_callback(const SSL *s, int where, int ret)
{
    const char *str;
    int w;
    w=where& ~SSL_ST_MASK;
    if (w & SSL_ST_CONNECT) str="SSL_connect";
    else if (w & SSL_ST_ACCEPT) str="SSL_accept";
    else str="undefined";
    if (where & SSL_CB_LOOP)
    {
        debugprintf("%s:%s\n",str,SSL_state_string_long(s));
    }
    else if (where & SSL_CB_ALERT)
    {
        str=(where & SSL_CB_READ)?"read":"write";
        debugprintf("SSL3 alert %s:%s:%s\n",
                str,
                SSL_alert_type_string_long(ret),
                SSL_alert_desc_string_long(ret));
    }
    else if (where & SSL_CB_EXIT)
    {
        if (ret == 0)
            debugprintf("%s:failed in %s\n",
                    str,SSL_state_string_long(s));
        else if (ret < 0)
        {
            debugprintf("%s:error in %s\n",
                    str,SSL_state_string_long(s));
        }
    }
}

SecureSocket::SecureSocket(std::weak_ptr<Kite::EventLoop> ev)
    : p(new SecureSocketPrivate(ev))
{
    static bool sslIsInit = false;
    if (!sslIsInit) {
        SSL_load_error_strings();
        SSL_library_init();
        ERR_load_BIO_strings();
        OpenSSL_add_all_algorithms();
    }
    p->p = this;
    p->ssl_ctx = 0;
    p->bio     = 0;
    p->ssl     = 0;
    p->state   = Disconnected;

    // init ssl context
    p->ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (p->ssl_ctx == NULL)
    {
        ERR_print_errors_fp(stderr);
        return;
    }


    SSL_CTX_set_info_callback(p->ssl_ctx, apps_ssl_info_callback);

}

SecureSocket::~SecureSocket()
{
    disconnect();


    if (p->bio)
        BIO_free_all(p->bio);

    if (p->ssl_ctx)
        SSL_CTX_free(p->ssl_ctx);

    delete p;
}

bool SecureSocket::setCaDir (const std::string &path)
{
    int r = SSL_CTX_load_verify_locations(p->ssl_ctx, NULL, path.c_str());
    if (r == 0) {
        return false;
    }
    return true;
}

bool SecureSocket::setCaFile(const std::string &path)
{
    int r = SSL_CTX_load_verify_locations(p->ssl_ctx, path.c_str(), NULL);
    if (r == 0) {
        return false;
    }
    return true;
}
bool SecureSocket::setClientCertificateFile(const std::string &path)
{
    int r = SSL_CTX_use_certificate_file(p->ssl_ctx, path.c_str(), SSL_FILETYPE_PEM);
    if (r == 0) {
        return false;
    }
    return true;
}

bool SecureSocket::setClientKeyFile(const std::string &path)
{
    int r = SSL_CTX_use_PrivateKey_file(p->ssl_ctx, path.c_str(), SSL_FILETYPE_PEM);
    if (r == 0) {
        return false;
    }
    return true;
}

void SecureSocket::disconnect()
{
    int fd = 0;
    BIO_get_fd(p->bio, &fd);
    if (fd != 0)
        p->evRemove(fd);

    if (p->ssl) {
        auto e = rebind_map.find(p->ssl);
        if (e != rebind_map.end())
            rebind_map.erase(e);
    }

    if (p->bio)
        BIO_ssl_shutdown(p->bio);

    if (p->state == Connected || p->state == Connecting)
        p->state = Disconnected;
    onDisconnected(p->state);
}

void SecureSocket::connect(const std::string &hostname, int port, uint64_t timeout, bool tls)
{
    p->useTls = tls;
    if (timeout > 0) {
        p->hasTimeout = true;
        p->connectionTimout.reset(timeout);
    }
    p->state   = Connecting;
    int r = 0;

    if (p->useTls) {
        p->bio = BIO_new_ssl_connect(p->ssl_ctx);
    } else {
        p->bio = BIO_new_connect(hostname.c_str());
    }
    if (!p->bio) {
        p->errorMessage = "BIO_new_ssl_connect returned NULL";
        p->state        = SecureSetupError;
        disconnect();
        return;
    }

    BIO_set_nbio(p->bio, 1);

    if (p->useTls) {
        BIO_get_ssl(p->bio, &p->ssl);
        if (!(p->ssl)) {
            p->errorMessage = "BIO_get_ssl returned NULL";
            p->state        = SecureSetupError;
            disconnect();
            return;
        }

        rebind_map.insert(std::make_pair(p->ssl, this));
        auto cb = [](SSL *ssl, X509 **x509, EVP_PKEY **pkey) {
            SecureSocket *that = rebind_map[ssl];
            that->p->errorMessage = "Client Certificate Requested\n";
            that->p->state = SecureClientCertificateRequired;
            that->disconnect();
            return 0;
        };
        SSL_CTX_set_client_cert_cb(p->ssl_ctx, cb);


        SSL_set_mode(p->ssl, SSL_MODE_AUTO_RETRY);
    }
    BIO_set_conn_hostname(p->bio, hostname.c_str());
    BIO_set_conn_int_port(p->bio, &port);

    p->d_connect();
}
void SecureSocketPrivate::d_connect()
{
    if (hasTimeout) {
        if (connectionTimout.expires() < 1) {
            errorMessage = "Connection timed out";
            state        = SecureSocket::TransportErrror;
            p->disconnect();
            return;
        }
    }
    int r  = BIO_do_connect(bio);
    if (r < 1) {
        if (BIO_should_retry(bio)) {
            // rety imidiately.
            // this seems how to do it properly, but i cant get it working:
            // https://github.com/jmckaskill/bio_poll/blob/master/poll.c
            // probably because BIO_get_fd is garbage before connect?
            Timer::later(ev(), [this](){
                    d_connect();
                    return false;
                    }, 100, "BIO_should_retry");
            return;
        }
        if (state != SecureSocket::SecureClientCertificateRequired) {
            const char *em = ERR_reason_error_string(ERR_get_error());
            debugprintf( "BIO_new_ssl_connect failed: %u (0x%x)\n", r, r);
            debugprintf( "Error: %s\n", em);
            debugprintf( "%s\n", ERR_error_string(ERR_get_error(), NULL));
            debugprintf("p_ssl state: %s\n",SSL_state_string_long(ssl));
            ERR_print_errors_fp(stderr);
            errorMessage = em ? em : "??";
            state        = SecureSocket::TransportErrror;
        }
        p->disconnect();
        return;
    }

    if (useTls) {
        auto result = SSL_get_verify_result(ssl);
        if (result != X509_V_OK) {
            std::stringstream str;
            str << "Secure Peer Verification Errror " << result;
            errorMessage = str.str();
            state        = SecureSocket::SecurePeerNotVerified;
            p->disconnect();
            return;
        }
    }

    int fd = 0;
    BIO_get_fd(bio, &fd);
    if (fd == 0) {
        errorMessage = "BIO_get_fd returned 0";
        state        = SecureSocket::SecureSetupError;
        p->disconnect();
        return;
    }
    evAdd(fd);


    state        = SecureSocket::Connected;
    p->onConnected();
}

int SecureSocket::write(const char *data, int len)
{
    if (p->state != Connected)
        return 0;

    int r;
    do {
        r = BIO_write(p->bio, data, len);
    } while (r < 0 && BIO_should_retry(p->bio));
    return len;
}

int SecureSocket::read (char *data, int len)
{
    if (p->state != Connected)
        return 0;
    int e = BIO_read(p->bio, data, len);
    if (e > -1)
        return e;
    if (BIO_should_retry(p->bio))
        return -1;
    return -2;
}

const std::string &SecureSocket::errorMessage() const
{
    return p->errorMessage;
}

void SecureSocket::flush()
{
    BIO_flush(p->bio);
}

const std::shared_ptr<EventLoop> SecureSocket::ev() const
{
    return p->ev();
}
