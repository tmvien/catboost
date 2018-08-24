#include "https.h"

#include "details.h"
#include "factory.h"
#include "http_common.h"
#include "jobqueue.h"
#include "location.h"
#include "multi.h"
#include "pipequeue.h"
#include "utils.h"

#include <contrib/libs/openssl/include/openssl/ssl.h>
#include <contrib/libs/openssl/include/openssl/err.h>
#include <contrib/libs/openssl/include/openssl/bio.h>
#include <contrib/libs/openssl/include/openssl/x509v3.h>

#include <library/openssl/init/init.h>
#include <library/coroutine/listener/listen.h>
#include <library/dns/cache.h>
#include <library/http/misc/parsed_request.h>
#include <library/http/misc/httpcodes.h>
#include <library/http/io/stream.h>

#include <util/generic/cast.h>
#include <util/generic/list.h>
#include <util/generic/utility.h>
#include <util/network/address.h>
#include <util/network/ip.h>
#include <util/network/socket.h>
#include <util/stream/str.h>
#include <util/stream/zlib.h>
#include <util/string/cast.h>
#include <util/string/builder.h>
#include <util/system/condvar.h>
#include <util/system/error.h>
#include <util/thread/pool.h>

#if defined(_unix_)
#include <sys/ioctl.h>
#endif

#if defined(_linux_)
#include <linux/sockios.h>
#define FIONWRITE SIOCOUTQ
#endif

using namespace NDns;
using namespace NAddr;

namespace NNeh {
    TString THttpsOptions::CAFile;
    TString THttpsOptions::CAPath;
    TString THttpsOptions::ClientCertificate;
    TString THttpsOptions::ClientPrivateKey;
    TString THttpsOptions::ClientPrivateKeyPassword;
    bool THttpsOptions::EnableSslServerDebug = false;
    bool THttpsOptions::EnableSslClientDebug = false;
    bool THttpsOptions::CheckCertificateHostname = false;
    THttpsOptions::TVerifyCallback THttpsOptions::ClientVerifyCallback = nullptr;
    THttpsOptions::TPasswordCallback THttpsOptions::KeyPasswdCallback = nullptr;

    bool THttpsOptions::Set(TStringBuf name, TStringBuf value) {
#define YNDX_NEH_HTTPS_TRY_SET(optName)                 \
    if (name == AsStringBuf(#optName)) {                \
        optName = FromString<decltype(optName)>(value); \
        return true;                                    \
    }

        YNDX_NEH_HTTPS_TRY_SET(CAFile);
        YNDX_NEH_HTTPS_TRY_SET(CAPath);
        YNDX_NEH_HTTPS_TRY_SET(ClientCertificate);
        YNDX_NEH_HTTPS_TRY_SET(ClientPrivateKey);
        YNDX_NEH_HTTPS_TRY_SET(ClientPrivateKeyPassword);
        YNDX_NEH_HTTPS_TRY_SET(EnableSslServerDebug);
        YNDX_NEH_HTTPS_TRY_SET(EnableSslClientDebug);
        YNDX_NEH_HTTPS_TRY_SET(CheckCertificateHostname);

#undef YNDX_NEH_HTTPS_TRY_SET

        return false;
    }
}

namespace NNeh {
    namespace NHttps {
        namespace {
            // force ssl_write/ssl_read functions to return this value via BIO_method_read/write that means request is canceled
            static constexpr int SSL_RVAL_TIMEOUT = -42;

            struct TInputConnections {
                TInputConnections()
                    : Counter(0)
                    , MaxUnusedConnKeepaliveTimeout(120)
                    , MinUnusedConnKeepaliveTimeout(10)
                {
                }

                inline size_t ExceedSoftLimit() const noexcept {
                    return NHttp::TFdLimits::ExceedLimit(Counter.Val(), Limits.Soft);
                }

                inline size_t ExceedHardLimit() const noexcept {
                    return NHttp::TFdLimits::ExceedLimit(Counter.Val(), Limits.Hard);
                }

                inline size_t DeltaLimit() const noexcept {
                    return Limits.Delta();
                }

                unsigned UnusedConnKeepaliveTimeout() const {
                    if (size_t e = ExceedSoftLimit()) {
                        size_t d = DeltaLimit();
                        size_t leftAvailableFd = NHttp::TFdLimits::ExceedLimit(d, e);
                        unsigned r = static_cast<unsigned>(MaxUnusedConnKeepaliveTimeout * leftAvailableFd / (d + 1));
                        return Max(r, (unsigned)MinUnusedConnKeepaliveTimeout);
                    }
                    return MaxUnusedConnKeepaliveTimeout;
                }

                void SetFdLimits(size_t soft, size_t hard) {
                    Limits.Soft = soft;
                    Limits.Hard = hard;
                }

                NHttp::TFdLimits Limits;
                TAtomicCounter Counter;
                volatile unsigned MaxUnusedConnKeepaliveTimeout; //in seconds
                volatile unsigned MinUnusedConnKeepaliveTimeout; //in seconds
            };

            TInputConnections* InputConnections() {
                return Singleton<TInputConnections>();
            }

            struct TSharedSocket: public TSocketHolder, public TAtomicRefCount<TSharedSocket> {
                inline TSharedSocket(TSocketHolder& s)
                    : TSocketHolder(s.Release())
                {
                    InputConnections()->Counter.Inc();
                }

                ~TSharedSocket() {
                    InputConnections()->Counter.Dec();
                }
            };

            using TSocketRef = TIntrusivePtr<TSharedSocket>;

            struct TX509Deleter {
                static void Destroy(X509* cert) {
                    X509_free(cert);
                }
            };
            using TX509Holder = THolder<X509, TX509Deleter>;

            struct TSslSessionDeleter {
                static void Destroy(SSL_SESSION* sess) {
                    SSL_SESSION_free(sess);
                }
            };
            using TSslSessionHolder = THolder<SSL_SESSION, TSslSessionDeleter>;

            struct TSslDeleter {
                static void Destroy(SSL* ssl) {
                    SSL_free(ssl);
                }
            };
            using TSslHolder = THolder<SSL, TSslDeleter>;

            struct TBIODeleter {
                static void Destroy(BIO* bio) {
                    BIO_free(bio);
                }
            };
            using TBIOHolder = TAutoPtr<BIO, TBIODeleter>;

            // read from bio and write via operator<<() to dst
            template <typename T>
            class TBIOInput {
            public:
                TBIOInput(T& dst)
                    : Dst_(dst)
                    , BIO_(BIO_new(&Ops_))
                {
                    BIO_->ptr = this;
                }

                operator BIO*() {
                    return BIO_.Get();
                }

            private:
                T& Dst_;
                TBIOHolder BIO_;

            private:
                static int BIOWriteMethod(BIO* bio, const char* buf, int len) {
                    if (!bio->ptr) {
                        return -1;
                    }
                    TBIOInput* in = static_cast<TBIOInput*>(bio->ptr);
                    in->Dst_ << TStringBuf(buf, len);
                    return len;
                }

                static int CreateMethod(BIO* bi) {
                    bi->init = 1;
                    bi->num = 0;
                    bi->ptr = nullptr;
                    bi->flags = 0;
                    return 1;
                }

                static long CtrlMethod(BIO* /*b*/, int cmd, long /*num*/, void* /*ptr*/) {
                    if (BIO_CTRL_FLUSH == cmd) {
                        return 1;
                    }
                    return 0;
                }

                static int DestroyMethod(BIO* b) {
                    if (!b) {
                        return 0;
                    }
                    b->ptr = nullptr;
                    b->init = 0;
                    b->flags = 0;
                    return 1;
                }

                static BIO_METHOD Ops_;
            };

            template <typename T>
            BIO_METHOD TBIOInput<T>::Ops_ = {
                (100 | 0x400),
                "TBIOInput",
                TBIOInput<T>::BIOWriteMethod,
                nullptr, // read
                nullptr, // puts
                nullptr, // gets
                TBIOInput<T>::CtrlMethod,
                TBIOInput<T>::CreateMethod,
                TBIOInput<T>::DestroyMethod,
                nullptr};
        }

        class TSslClientIOStream;
        struct TSslSocketHolder : public TSocketHolder {
            inline TSslSocketHolder()
                : TSocketHolder()
                , SslIOStream(nullptr)
            {
            }
            inline TSslSocketHolder(TSslSocketHolder& s)
                : TSocketHolder(s.Release())
                , SslIOStream(nullptr)
            {
            }
            inline TSslSocketHolder(SOCKET fd)
                : TSocketHolder(fd)
                , SslIOStream(nullptr)
            {
            }
            ~TSslSocketHolder() {
            }
            THolder<TSslClientIOStream> SslIOStream;
        };


        class TSslException: public yexception {
        public:
            TSslException() {
            }

            TSslException(TStringBuf f) {
                *this << f << Endl;
                InitErr();
            }

            TSslException(TStringBuf f, const SSL* ssl, int ret) {
                *this << f << AsStringBuf(" error type: ");
                const int etype = SSL_get_error(ssl, ret);
                switch (etype) {
                    case SSL_ERROR_ZERO_RETURN:
                        *this << AsStringBuf("SSL_ERROR_ZERO_RETURN");
                        break;
                    case SSL_ERROR_WANT_READ:
                        *this << AsStringBuf("SSL_ERROR_WANT_READ");
                        break;
                    case SSL_ERROR_WANT_WRITE:
                        *this << AsStringBuf("SSL_ERROR_WANT_WRITE");
                        break;
                    case SSL_ERROR_WANT_CONNECT:
                        *this << AsStringBuf("SSL_ERROR_WANT_CONNECT");
                        break;
                    case SSL_ERROR_WANT_ACCEPT:
                        *this << AsStringBuf("SSL_ERROR_WANT_ACCEPT");
                        break;
                    case SSL_ERROR_WANT_X509_LOOKUP:
                        *this << AsStringBuf("SSL_ERROR_WANT_X509_LOOKUP");
                        break;
                    case SSL_ERROR_SYSCALL:
                        *this << AsStringBuf("SSL_ERROR_SYSCALL ret: ") << ret << AsStringBuf(", errno: ") << errno;
                        break;
                    case SSL_ERROR_SSL:
                        *this << AsStringBuf("SSL_ERROR_SSL");
                        break;
                }
                *this << ' ';
                InitErr();
            }

        private:
            void InitErr() {
                TBIOInput<TSslException> bio(*this);
                ERR_print_errors(bio);
            }
        };

        namespace {
            enum EMatchResult {
                MATCH_FOUND,
                NO_MATCH,
                NO_EXTENSION,
                ERROR
            };
            static bool EqualNoCase(TStringBuf a, TStringBuf b) {
                return (+a == +b) && a.ToString().to_lower() == b.ToString().to_lower();
            }
            static bool MatchDomainName(TStringBuf tmpl, TStringBuf name) {
                // match wildcards only in the left-most part
                // do not support (optional according to RFC) partial wildcards (ww*.yandex.ru)
                // see RFC-6125
                TStringBuf tmplRest = tmpl;
                TStringBuf tmplFirst = tmplRest.NextTok('.');
                if (tmplFirst == "*") {
                    tmpl = tmplRest;
                    name.NextTok('.');
                }
                return EqualNoCase(tmpl, name);
            }

            static EMatchResult MatchCertAltNames(X509* cert, TStringBuf hostname) {
                EMatchResult result = NO_MATCH;
                STACK_OF(GENERAL_NAME)* names = (STACK_OF(GENERAL_NAME)*)X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, NULL);
                if (!names) {
                    return NO_EXTENSION;
                }

                int namesCt = sk_GENERAL_NAME_num(names);
                for (int i = 0; i < namesCt; ++i) {
                    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);

                    if (name->type == GEN_DNS) {
                        TStringBuf dnsName((char*)ASN1_STRING_data(name->d.dNSName), ASN1_STRING_length(name->d.dNSName));
                        if (MatchDomainName(dnsName, hostname)) {
                            result = MATCH_FOUND;
                            break;
                        }
                    }
                }
                sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
                return result;
            }

            static EMatchResult MatchCertCommonName(X509* cert, TStringBuf hostname) {
                int commonNameLoc = X509_NAME_get_index_by_NID(X509_get_subject_name(cert), NID_commonName, -1);
                if (commonNameLoc < 0) {
                    return ERROR;
                }

                X509_NAME_ENTRY* commonNameEntry = X509_NAME_get_entry(X509_get_subject_name(cert), commonNameLoc);
                if (!commonNameEntry) {
                    return ERROR;
                }

                ASN1_STRING* commonNameAsn1 = X509_NAME_ENTRY_get_data(commonNameEntry);
                if (!commonNameAsn1) {
                    return ERROR;
                }

                TStringBuf commonName((char*)ASN1_STRING_data(commonNameAsn1), ASN1_STRING_length(commonNameAsn1));

                return MatchDomainName(commonName, hostname)
                           ? MATCH_FOUND
                           : NO_MATCH;
            }

            static bool CheckCertHostname(X509* cert, TStringBuf hostname) {
                switch (MatchCertAltNames(cert, hostname)) {
                    case MATCH_FOUND:
                        return true;
                        break;
                    case NO_EXTENSION:
                        return MatchCertCommonName(cert, hostname) == MATCH_FOUND;
                        break;
                    default:
                        return false;
                }
            }

            static void ParseUserInfo(const TParsedLocation& loc, TString& cert, TString& pvtKey) {
                if (!loc.UserInfo) {
                    return;
                }

                TStringBuf kws = loc.UserInfo;
                while (kws) {
                    TStringBuf name = kws.NextTok('=');
                    TStringBuf value = kws.NextTok(';');
                    if (AsStringBuf("cert") == name) {
                        cert = value;
                    } else if (AsStringBuf("key") == name) {
                        pvtKey = value;
                    }
                }
            }

            struct TSSLInit {
                inline TSSLInit() {
                    InitOpenSSL();
                }
            } SSL_INIT;
        }

        static inline void PrepareSocket(SOCKET s) {
            SetNoDelay(s, true);
        }

        class TConnCache;
        static TConnCache* SocketCache();

        class TSslClientIOStream;
        class TConnCache: public IThreadPool::IThreadAble {
        public:
            typedef TAutoLockFreeQueue<TSslSocketHolder> TConnList;
            typedef TAutoPtr<TSslSocketHolder> TSocketRef;

            struct TConnection {
                inline TConnection(TSocketRef& s, bool reUsed, const TResolvedHost* host) noexcept
                    : Socket(s)
                    , ReUsed(reUsed)
                    , Host(host)
                {
                    SocketCache()->ActiveSockets.Inc();
                }

                inline ~TConnection() {
                    if (!!Socket) {
                        SocketCache()->ActiveSockets.Dec();
                    }
                }

                SOCKET Fd() {
                    return *Socket;
                }

                TSslClientIOStream* SslStream() {
                    return Socket->SslIOStream.Get();
                }

                void SetSslStream(TSslClientIOStream* io) {
                    Socket->SslIOStream.Reset(io);
                }

            protected:
                friend class TConnCache;
                TSocketRef Socket;

            public:
                const bool ReUsed;
                const TResolvedHost* Host;
            };

            TConnCache()
                : InPurging_(0)
                , MaxConnId_(0)
                , Shutdown_(false)
            {
                T_ = SystemThreadPool()->Run(this);
            }

            ~TConnCache() override {
                {
                    TGuard<TMutex> g(PurgeMutex_);

                    Shutdown_ = true;
                    CondPurge_.Signal();
                }

                T_->Join();
            }

            //used for forwarding filling cache
            class TConnector: public IJob {
            public:
                //create fresh connection
                TConnector(const TResolvedHost* host)
                    : Host_(host)
                {
                }

                //continue connecting exist socket
                TConnector(const TResolvedHost* host, TSocketRef& s)
                    : Host_(host)
                    , S_(s)
                {
                }

                void DoRun(TCont* c) override {
                    THolder<TConnector> This(this);

                    try {
                        if (!S_) {
                            TSocketRef res(new TSslSocketHolder());

                            for (TNetworkAddress::TIterator it = Host_->Addr.Begin(); it != Host_->Addr.End(); ++it) {
                                int ret = c->Connect(*res, *it, TDuration::MilliSeconds(300).ToDeadLine());

                                if (!ret) {
                                    TConnection tc(res, false, Host_);
                                    SocketCache()->Release(tc);
                                    return;
                                }

                                if (ret == ECANCELED) {
                                    return;
                                }
                            }
                        } else {
                            if (!c->PollT(*S_, CONT_POLL_WRITE, TDuration::MilliSeconds(300))) {
                                TConnection tc(S_, false, Host_);
                                SocketCache()->Release(tc);
                            }
                        }
                    } catch (...) {
                    }
                }

            private:
                const TResolvedHost* Host_;
                TSocketRef S_;
            };

            TConnection* Connect(TCont* c, const TString& msgAddr, const TResolvedHost* addr, TErrorRef* error) {
                if (ExceedHardLimit()) {
                    if (error) {
                        *error = new TError("neh::https output connections limit reached", TError::TType::UnknownType);
                    }
                    return nullptr;
                }

                TSocketRef res;
                TConnList& connList = ConnList(addr);

                while (connList.Dequeue(&res)) {
                    CachedSockets.Dec();

                    if (TCont::SocketNotClosedByOtherSide(*res)) {
                        if (connList.Size() == 0) {
                            //available connections exhausted - try create yet one (reserve)
                            TAutoPtr<IJob> job(new TConnector(addr));

                            if (c) {
                                try {
                                    c->Executor()->Create(*job, "https-con");
                                    Y_UNUSED(job.Release());
                                } catch (...) {
                                }
                            } else {
                                JobQueue()->Schedule(job);
                            }
                        }
                        return new TConnection(res, true, addr);
                    }
                }

                if (!c) {
                    if (error) {
                        *error = new TError("directo connection failed");
                    }
                    return nullptr;
                }

                try {
                    //run reserve/concurrent connecting
                    TAutoPtr<IJob> job(new TConnector(addr));

                    c->Executor()->Create(*job, "https-con");
                    Y_UNUSED(job.Release());
                } catch (...) {
                }

                TNetworkAddress::TIterator ait = addr->Addr.Begin();

                res.Reset(new TSslSocketHolder(c->Socket(*ait)));
                const TInstant now(TInstant::Now());
                const TInstant deadline(now + TDuration::Seconds(10));
                TDuration delay = TDuration::MilliSeconds(8);
                TInstant checkpoint = Min(deadline, now + delay);
                int ret = c->ConnectD(*res, ait->ai_addr, ait->ai_addrlen, checkpoint);

                if (ret) {
                    do {
                        if ((ret == ETIMEDOUT || ret == EINTR) && checkpoint < deadline) {
                            delay += delay;
                            checkpoint = Min(deadline, now + delay);

                            TSocketRef res2;

                            if (connList.Dequeue(&res2)) {
                                CachedSockets.Dec();

                                if (TCont::SocketNotClosedByOtherSide(*res2)) {
                                    try {
                                        TAutoPtr<IJob> job(new TConnector(addr, res));

                                        c->Executor()->Create(*job, "https-con");
                                        Y_UNUSED(job.Release());
                                    } catch (...) {
                                    }

                                    res = res2;

                                    break;
                                }
                            }
                        } else {
                            if (error) {
                                *error = new TError(TStringBuilder() << AsStringBuf("can not connect to ") << msgAddr);
                            }
                            return nullptr;
                        }
                    } while (ret = c->PollD(*res, CONT_POLL_WRITE, checkpoint));
                }

                PrepareSocket(*res);

                return new TConnection(res, false, addr);
            }

            inline void Release(TConnection& conn) {
                if (!ExceedHardLimit()) {
                    size_t maxConnId = MaxConnId_;

                    while (maxConnId < conn.Host->Id) {
                        AtomicCas(&MaxConnId_, conn.Host->Id, maxConnId);
                        maxConnId = MaxConnId_;
                    }

                    CachedSockets.Inc();
                    ActiveSockets.Dec();

                    ConnList(conn.Host).Enqueue(conn.Socket);
                }

                if (CachedSockets.Val() && ExceedSoftLimit()) {
                    SuggestPurgeCache();
                }
            }

            void SetFdLimits(size_t soft, size_t hard) {
                Limits.Soft = soft;
                Limits.Hard = hard;
            }

        private:
            void SuggestPurgeCache() {
                if (AtomicTryLock(&InPurging_)) {
                    //evaluate the usefulness of purging the cache
                    //если в кеше мало соединений (< MaxConnId_/16 или 64), не чистим кеш
                    if ((size_t)CachedSockets.Val() > (Min((size_t)MaxConnId_, (size_t)1024U) >> 4)) {
                        //по мере приближения к hardlimit нужда в чистке cache приближается к 100%
                        size_t closenessToHardLimit256 = ((ActiveSockets.Val() + 1) << 8) / (Limits.Delta() + 1);
                        //чем больше соединений в кеше, а не в работе, тем менее нужен кеш (можно его почистить)
                        size_t cacheUselessness256 = ((CachedSockets.Val() + 1) << 8) / (ActiveSockets.Val() + 1);

                        //итого, - пороги срабатывания:
                        //при достижении soft-limit, если соединения в кеше, а не в работе
                        //на полпути от soft-limit к hard-limit, если в кеше больше половины соединений
                        //при приближении к hardlimit пытаться почистить кеш почти постоянно
                        if ((closenessToHardLimit256 + cacheUselessness256) >= 256U) {
                            TGuard<TMutex> g(PurgeMutex_);

                            CondPurge_.Signal();
                            return; //memo: thread MUST unlock InPurging_ (see DoExecute())
                        }
                    }
                    AtomicUnlock(&InPurging_);
                }
            }

            void DoExecute() override {
                while (true) {
                    {
                        TGuard<TMutex> g(PurgeMutex_);

                        if (Shutdown_)
                            return;

                        CondPurge_.WaitI(PurgeMutex_);
                    }

                    PurgeCache();

                    AtomicUnlock(&InPurging_);
                }
            }

            inline void OnPurgeSocket(ui64& processed) {
                CachedSockets.Dec();
                if ((processed++ & 0x3f) == 0) {
                    //suspend execution every 64 processed socket (clean rate ~= 6400 sockets/sec)
                    Sleep(TDuration::MilliSeconds(10));
                }
            }

            void PurgeCache() noexcept {
                //try remove at least ExceedSoftLimit() oldest connections from cache
                //вычисляем долю кеша, которую нужно почистить (в 256 долях) (но не менее 1/32 кеша)
                size_t frac256 = Min(size_t(Max(size_t(256U / 32U), (ExceedSoftLimit() << 8) / (CachedSockets.Val() + 1))), (size_t)256U);
                TSocketRef tmp;

                ui64 processed = 0;
                for (size_t i = 0; i < MaxConnId_ && !Shutdown_; i++) {
                    TConnList& tc = Lst_.Get(i);
                    if (size_t qsize = tc.Size()) {
                        //в каждой очереди чистим вычисленную долю
                        size_t purgeCounter = ((qsize * frac256) >> 8);

                        if (!purgeCounter && qsize) {
                            if (qsize <= 2) {
                                TSocketRef res;
                                if (tc.Dequeue(&res)) {
                                    if (TCont::SocketNotClosedByOtherSide(*res)) {
                                        tc.Enqueue(res);
                                    } else {
                                        OnPurgeSocket(processed);
                                    }
                                }
                            } else {
                                purgeCounter = 1;
                            }
                        }
                        while (purgeCounter-- && tc.Dequeue(&tmp)) {
                            OnPurgeSocket(processed);
                        }
                    }
                }
            }

            inline TConnList& ConnList(const TResolvedHost* addr) {
                return Lst_.Get(addr->Id);
            }

            inline size_t TotalSockets() const noexcept {
                return ActiveSockets.Val() + CachedSockets.Val();
            }

            inline size_t ExceedSoftLimit() const noexcept {
                return NHttp::TFdLimits::ExceedLimit(TotalSockets(), Limits.Soft);
            }

            inline size_t ExceedHardLimit() const noexcept {
                return NHttp::TFdLimits::ExceedLimit(TotalSockets(), Limits.Hard);
            }

            NHttp::TFdLimits Limits;
            TAtomicCounter ActiveSockets;
            TAtomicCounter CachedSockets;

            NHttp::TLockFreeSequence<TConnList> Lst_;

            TAtomic InPurging_;
            volatile size_t MaxConnId_;

            TAutoPtr<IThreadPool::IThread> T_;
            TCondVar CondPurge_;
            TMutex PurgeMutex_;
            volatile bool Shutdown_;
        };

        class TSslCtx: public TThrRefBase {
        protected:
            TSslCtx()
                : SslCtx_(nullptr)
            {
            }

        public:
            virtual ~TSslCtx() {
                SSL_CTX_free(SslCtx_);
            }

            operator SSL_CTX*() {
                return SslCtx_;
            }

        protected:
            SSL_CTX* SslCtx_;
        };
        using TSslCtxPtr = TIntrusivePtr<TSslCtx>;

        class TSslCtxServer: public TSslCtx {
            struct TPasswordCallbackUserData {
                TParsedLocation Location;
                TString         CertFileName;
                TString         KeyFileName;
            };
            class TUserDataHolder {
            public:
                TUserDataHolder(SSL_CTX* ctx, const TParsedLocation& location, const TString& certFileName, const TString& keyFileName)
                    : SslCtx_(ctx)
                    , Data_{location, certFileName, keyFileName}
                {
                    SSL_CTX_set_default_passwd_cb_userdata(SslCtx_, &Data_);
                }
                ~TUserDataHolder() {
                    SSL_CTX_set_default_passwd_cb_userdata(SslCtx_, nullptr);
                }
            private:
                SSL_CTX* SslCtx_;
                TPasswordCallbackUserData Data_;
            };
        public:
            TSslCtxServer(const TParsedLocation& loc) {
                const SSL_METHOD* method = SSLv23_server_method();
                if (Y_UNLIKELY(!method)) {
                    ythrow TSslException(AsStringBuf("SSLv23_server_method"));
                }

                SslCtx_ = SSL_CTX_new(method);
                if (Y_UNLIKELY(!SslCtx_)) {
                    ythrow TSslException(AsStringBuf("SSL_CTX_new(server)"));
                }

                TString cert, key;
                ParseUserInfo(loc, cert, key);

                TUserDataHolder holder(SslCtx_, loc, cert, key);

                SSL_CTX_set_default_passwd_cb(SslCtx_, [](char* buf, int size, int rwflag, void* userData) -> int {
                    Y_UNUSED(rwflag);
                    Y_UNUSED(userData);

                    if (THttpsOptions::KeyPasswdCallback == nullptr || userData == nullptr) {
                        return 0;
                    }

                    auto data = static_cast<TPasswordCallbackUserData*>(userData);
                    const auto& passwd = THttpsOptions::KeyPasswdCallback(data->Location, data->CertFileName, data->KeyFileName);

                    if (size < static_cast<int>(passwd.size())) {
                        return -1;
                    }

                    return passwd.copy(buf, size, 0);
                });

                if (!cert || !key) {
                    ythrow TSslException() << AsStringBuf("no certificate or private key is specified for server");
                }

                if (1 != SSL_CTX_use_certificate_chain_file(SslCtx_, ~cert)) {
                    ythrow TSslException(AsStringBuf("SSL_CTX_use_certificate_chain_file (server)"));
                }

                if (1 != SSL_CTX_use_PrivateKey_file(SslCtx_, ~key, SSL_FILETYPE_PEM)) {
                    ythrow TSslException(AsStringBuf("SSL_CTX_use_PrivateKey_file (server)"));
                }

                if (1 != SSL_CTX_check_private_key(SslCtx_)) {
                    ythrow TSslException(AsStringBuf("SSL_CTX_check_private_key (server)"));
                }
            }
        };

        class TSslCtxClient: public TSslCtx {
        public:
            TSslCtxClient() {
                const SSL_METHOD* method = SSLv23_client_method();
                if (Y_UNLIKELY(!method)) {
                    ythrow TSslException(AsStringBuf("SSLv23_client_method"));
                }

                SslCtx_ = SSL_CTX_new(method);
                if (Y_UNLIKELY(!SslCtx_)) {
                    ythrow TSslException(AsStringBuf("SSL_CTX_new(client)"));
                }

                const TString& caFile = THttpsOptions::CAFile;
                const TString& caPath = THttpsOptions::CAPath;
                if (caFile || caPath) {
                    if (!SSL_CTX_load_verify_locations(SslCtx_, caFile ? ~caFile : nullptr, caPath ? ~caPath : nullptr)) {
                        ythrow TSslException(AsStringBuf("SSL_CTX_load_verify_locations(client)"));
                    }
                }

                SSL_CTX_set_options(SslCtx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
                if (THttpsOptions::ClientVerifyCallback) {
                    SSL_CTX_set_verify(SslCtx_, SSL_VERIFY_PEER, THttpsOptions::ClientVerifyCallback);
                } else {
                    SSL_CTX_set_verify(SslCtx_, SSL_VERIFY_NONE, nullptr);
                }

                const TString& clientCertificate = THttpsOptions::ClientCertificate;
                const TString& clientPrivateKey = THttpsOptions::ClientPrivateKey;
                if (clientCertificate && clientPrivateKey) {
                    SSL_CTX_set_default_passwd_cb(SslCtx_, [](char* buf, int size, int rwflag, void* userData) -> int {
                        Y_UNUSED(rwflag);
                        Y_UNUSED(userData);

                        const TString& clientPrivateKeyPwd = THttpsOptions::ClientPrivateKeyPassword;
                        if (!clientPrivateKeyPwd) {
                            return 0;
                        }
                        if (size < static_cast<int>(clientPrivateKeyPwd.size())) {
                            return -1;
                        }

                        return clientPrivateKeyPwd.copy(buf, size, 0);
                    });
                    if (1 != SSL_CTX_use_certificate_chain_file(SslCtx_, clientCertificate.c_str())) {
                        ythrow TSslException(AsStringBuf("SSL_CTX_use_certificate_chain_file (client)"));
                    }
                    if (1 != SSL_CTX_use_PrivateKey_file(SslCtx_, clientPrivateKey.c_str(), SSL_FILETYPE_PEM)) {
                        ythrow TSslException(AsStringBuf("SSL_CTX_use_PrivateKey_file (client)"));
                    }
                    if (1 != SSL_CTX_check_private_key(SslCtx_)) {
                        ythrow TSslException(AsStringBuf("SSL_CTX_check_private_key (client)"));
                    }
                } else if (clientCertificate || clientPrivateKey) {
                    ythrow TSslException() << AsStringBuf("both certificate and private key must be specified for client");
                }
            }

            static TSslCtxClient& Instance() {
                return *Singleton<TSslCtxClient>();
            }
        };

        class TContBIO {
        private:
            BIO* CreateBIO() {
                TBIOHolder bio(BIO_new(&CarrierSslOps_));
                if (Y_UNLIKELY(!bio)) {
                    ythrow TSslException(AsStringBuf("BIO_new"));
                }
                bio->ptr = this;
                return bio.Release();
            }

        public:
            TContBIO(SOCKET s, const TAtomicBool* canceled = nullptr)
                : Timeout_(TDuration::MicroSeconds(10000))
                , S_(s)
                , Canceled_(canceled)
                , Cont_(nullptr)
                , Bio_(CreateBIO())
            {
            }

            void SetCanceledPtr(const TAtomicBool* canceled) {
                Canceled_ = canceled;
            }

            SOCKET Socket() {
                return S_;
            }

            BIO* Bio() {
                return Bio_;
            }

            int PollT(int what, const TDuration& timeout) {
                return Cont_->PollT(Socket(), what, timeout);
            }

            void WaitUntilWritten() {
#if defined(FIONWRITE)
                if (Y_LIKELY(Cont_)) {
                    int err;
                    int nbytes = Max<int>();
                    TDuration tout = TDuration::MilliSeconds(10);

                    while (((err = ioctl(S_, FIONWRITE, &nbytes)) == 0) && nbytes) {
                        err = Cont_->PollT(S_, CONT_POLL_READ, tout);

                        if (!err) {
                            //wait complete, cause have some data
                            break;
                        }

                        if (err != ETIMEDOUT) {
                            ythrow TSystemError(err) << AsStringBuf("request failed");
                        }

                        tout = tout * 2;
                    }

                    if (err) {
                        ythrow TSystemError() << AsStringBuf("ioctl() failed");
                    }
                } else {
                    ythrow TSslException() << AsStringBuf("No cont available");
                }
#endif
            }

            void AcquireCont(TCont* c) {
                Cont_ = c;
            }
            void ReleaseCont() {
                Cont_ = nullptr;
            }

        private:
            TDuration Timeout_;
            SOCKET S_;
            const TAtomicBool* Canceled_;
            TCont* Cont_;
            BIO* Bio_;

        private:
            static int BIOReadMethod(BIO* bio, char* buf, int len) {
                if (Y_UNLIKELY(!bio->ptr)) {
                    return -1;
                }

                TContBIO* conn = static_cast<TContBIO*>(bio->ptr);
                TCont* c = conn->Cont_;
                if (Y_UNLIKELY(!c)) {
                    return -1;
                }

                SOCKET fd = conn->S_;
                const TAtomicBool* canceled = conn->Canceled_;

                Cdbg << '[' << fd << AsStringBuf("] TSSLConnection::BIOReadMethod(") << len << ')' << Endl;

                if (!canceled) {
                    while (true) {
                        auto done = c->ReadI(fd, buf, len);
                        if (EAGAIN != done.Status()) {
                            return done.Processed();
                        }
                    }
                }

                while (true) {
                    if (*canceled) {
                        return SSL_RVAL_TIMEOUT;
                    }

                    TContIOStatus ioStat(c->ReadT(fd, buf, len, conn->Timeout_));
                    if (ioStat.Status() == ETIMEDOUT) {
                        //increase to 1.5 times every iteration (to 1sec floor)
                        conn->Timeout_ = TDuration::MicroSeconds(Min<ui64>(1000000, conn->Timeout_.MicroSeconds() + (conn->Timeout_.MicroSeconds() >> 1)));
                        continue;
                    }

                    return ioStat.Processed();
                }
            }

            static int BIOWriteMethod(BIO* bio, const char* buf, int len) {
                if (Y_UNLIKELY(!bio->ptr)) {
                    return -1;
                }

                auto* conn = static_cast<TContBIO*>(bio->ptr);
                if (Y_UNLIKELY(!conn->Cont_)) {
                    return -1;
                }

                Cdbg << '[' << conn->S_ << AsStringBuf("] TSSLConnection::BIOWriteMethod(") << len << ')' << Endl;

                while (true) {
                    auto done = conn->Cont_->WriteI(conn->S_, buf, len);
                    if (done.Status() != EAGAIN) {
                        return done.Checked();
                    }
                }
            }

            static int BIOCreateMethod(BIO* bi) {
                bi->init = 1;
                bi->num = 0;
                bi->ptr = nullptr;
                bi->flags = 0;
                return 1;
            }

            static long BIOCtrlMethod(BIO* /*bio*/, int cmd, long /*num*/, void* /*ptr*/) {
                if (BIO_CTRL_FLUSH == cmd) {
                    return 1;
                }
                return 0;
            }

            static int BIODestroyMethod(BIO* b) {
                if (!b) {
                    return 0;
                }
                b->ptr = nullptr;
                b->init = 0;
                b->flags = 0;
                return 1;
            }

            static BIO_METHOD CarrierSslOps_;
        };

        BIO_METHOD TContBIO::CarrierSslOps_ = {
            (100 | 0x400),
            "TContBIO",
            TContBIO::BIOWriteMethod,
            TContBIO::BIOReadMethod,
            nullptr,
            nullptr,
            TContBIO::BIOCtrlMethod,
            TContBIO::BIOCreateMethod,
            TContBIO::BIODestroyMethod,
            nullptr};

        class TSslIOStream: public IInputStream, public IOutputStream {
        protected:
            TSslIOStream(TSslCtx& sslCtx, TAutoPtr<TContBIO> connection)
                : Connection_(connection)
                , SslCtx_(sslCtx)
                , Ssl_(nullptr)
            {
            }

            virtual void Handshake() = 0;

        public:
            void SetCanceledPtr(const TAtomicBool *canceled) {
                Connection_->SetCanceledPtr(canceled);
            }

            void WaitUntilWritten() {
                if (Connection_) {
                    Connection_->WaitUntilWritten();
                }
            }

            int PollReadT(const TDuration& timeout) {
                if (!Connection_) {
                    return -1;
                }

                while (true) {
                    const int rpoll = Connection_->PollT(CONT_POLL_READ, timeout);
                    if (!Ssl_ || rpoll) {
                        return rpoll;
                    }

                    char c = 0;
                    const int rpeek = SSL_peek(Ssl_.Get(), &c, sizeof(c));
                    if (rpeek < 0) {
                        return -1;
                    } else if (rpeek > 0) {
                        return 0;
                    } else {
                        if ((SSL_get_shutdown(Ssl_.Get()) & SSL_RECEIVED_SHUTDOWN) != 0) {
                            Shutdown(); // wait until shutdown is finished
                            return EIO;
                        }
                    }
                }
            }

            void Shutdown() {
                if (Ssl_ && Connection_) {
                    for (size_t i = 0; i < 2; ++i) {
                        bool rval = SSL_shutdown(Ssl_.Get());
                        if (0 == rval) {
                            continue;
                        } else if (1 == rval) {
                            break;
                        }
                    }
                }
                Ssl_.Reset(nullptr);
            }

            inline void AcquireCont(TCont* c) {
                if (Y_UNLIKELY(!Connection_)) {
                    ythrow TSslException() << AsStringBuf("no connection provided");
                }

                Connection_->AcquireCont(c);
            }

            inline void ReleaseCont() {
                if (Connection_) {
                    Connection_->ReleaseCont();
                }
            }

            TContIOStatus WriteVectorI(const TList<IOutputStream::TPart>& vec) {
                for (const auto& p : vec) {
                    Write(p.buf, p.len);
                }
                return TContIOStatus::Success(vec.size());
            }

            SOCKET Socket() {
                if (Y_UNLIKELY(!Connection_)) {
                    ythrow TSslException() << AsStringBuf("no connection provided");
                }

                return Connection_->Socket();
            }

        private:
            void DoWrite(const void* buf, size_t len) override {
                if (Y_UNLIKELY(!Connection_)) {
                    ythrow TSslException() << AsStringBuf("DoWrite() no connection provided");
                }

                const int rval = SSL_write(Ssl_.Get(), buf, len);
                if (rval <= 0) {
                    ythrow TSslException(AsStringBuf("SSL_write"), Ssl_.Get(), rval);
                }
            }

            size_t DoRead(void* buf, size_t len) override {
                if (Y_UNLIKELY(!Connection_)) {
                    ythrow TSslException() << AsStringBuf("DoRead() no connection provided");
                }

                const int rval = SSL_read(Ssl_.Get(), buf, len);
                if (rval < 0) {
                    if (SSL_RVAL_TIMEOUT == rval) {
                        ythrow TSystemError(ECANCELED) << AsStringBuf(" http request canceled");
                    }
                    ythrow TSslException(AsStringBuf("SSL_read"), Ssl_.Get(), rval);
                } else if (0 == rval) {
                    if ((SSL_get_shutdown(Ssl_.Get()) & SSL_RECEIVED_SHUTDOWN) != 0) {
                        return rval;
                    } else {
                        const int err = SSL_get_error(Ssl_.Get(), rval);
                        if (SSL_ERROR_ZERO_RETURN != err) {
                            ythrow TSslException(AsStringBuf("SSL_read"), Ssl_.Get(), rval);
                        }
                    }
                }

                return static_cast<size_t>(rval);
            }

        protected:
            // just for ssl debug
            static void InfoCB(const SSL* s, int where, int ret) {
                TStringBuf str;
                const int w = where & ~SSL_ST_MASK;
                if (w & SSL_ST_CONNECT) {
                    str = AsStringBuf("SSL_connect");
                } else if (w & SSL_ST_ACCEPT) {
                    str = AsStringBuf("SSL_accept");
                } else {
                    str = AsStringBuf("undefined");
                }

                if (where & SSL_CB_LOOP) {
                    Cerr << str << ':' << SSL_state_string_long(s) << Endl;
                } else if (where & SSL_CB_ALERT) {
                    Cerr << AsStringBuf("SSL3 alert ") << ((where & SSL_CB_READ) ? AsStringBuf("read") : AsStringBuf("write")) << ' ' << SSL_alert_type_string_long(ret) << ':' << SSL_alert_desc_string_long(ret) << Endl;
                } else if (where & SSL_CB_EXIT) {
                    if (ret == 0) {
                        Cerr << str << AsStringBuf(":failed in ") << SSL_state_string_long(s) << Endl;
                    } else if (ret < 0) {
                        Cerr << str << AsStringBuf(":error in ") << SSL_state_string_long(s) << Endl;
                    }
                }
            }

        protected:
            THolder<TContBIO> Connection_;
            TSslCtx& SslCtx_;
            TSslHolder Ssl_;
        };

        class TContBIOWatcher {
        public:
            TContBIOWatcher(TSslIOStream& io, TCont* c) noexcept
                : IO_(io)
            {
                IO_.AcquireCont(c);
            }

            ~TContBIOWatcher() noexcept {
                IO_.ReleaseCont();
            }

        private:
            TSslIOStream& IO_;
        };

        class TSslClientIOStream: public TSslIOStream {
        public:
            TSslClientIOStream(TSslCtxClient& sslCtx, const TParsedLocation& loc, SOCKET s, const TAtomicBool* canceled)
                : TSslIOStream(sslCtx, new TContBIO(s, canceled))
                , Location_(loc)
            {
            }

            bool Initialized() const {
                return Ssl_.Get();
            }

            void Handshake() {
                Ssl_ = SSL_new(SslCtx_);
                if (THttpsOptions::EnableSslClientDebug) {
                    SSL_set_info_callback(Ssl_.Get(), InfoCB);
                }

                THolder<BIO> bio = Connection_->Bio();
                SSL_set_bio(Ssl_.Get(), bio.Get(), bio.Get());
                Y_UNUSED(bio.Release());

                const TString hostname(Location_.Host);
                const int rev = SSL_set_tlsext_host_name(Ssl_.Get(), ~hostname);
                if (Y_UNLIKELY(1 != rev)) {
                    ythrow TSslException(AsStringBuf("SSL_set_tlsext_host_name(client)"), Ssl_.Get(), rev);
                }

                TString cert, pvtKey;
                ParseUserInfo(Location_, cert, pvtKey);

                if (cert && (1 != SSL_use_certificate_file(Ssl_.Get(), ~cert, SSL_FILETYPE_PEM))) {
                    ythrow TSslException(AsStringBuf("SSL_use_certificate_file(client)"));
                }

                if (pvtKey) {
                    if (1 != SSL_use_PrivateKey_file(Ssl_.Get(), ~pvtKey, SSL_FILETYPE_PEM)) {
                        ythrow TSslException(AsStringBuf("SSL_use_PrivateKey_file(client)"));
                    }

                    if (1 != SSL_check_private_key(Ssl_.Get())) {
                        ythrow TSslException(AsStringBuf("SSL_check_private_key(client)"));
                    }
                }

                SSL_set_connect_state(Ssl_.Get());

                // TODO restore session if reconnect
                const int rval = SSL_do_handshake(Ssl_.Get());
                if (1 != rval) {
                    if (rval == SSL_RVAL_TIMEOUT) {
                        ythrow TSystemError(ECANCELED) << AsStringBuf("canceled");
                    } else {
                        ythrow TSslException(AsStringBuf("BIO_do_handshake(client)"), Ssl_.Get(), rval);
                    }
                }

                if (THttpsOptions::CheckCertificateHostname) {
                    TX509Holder peerCert = SSL_get_peer_certificate(Ssl_.Get());
                    if (!peerCert) {
                        ythrow TSslException(AsStringBuf("SSL_get_peer_certificate(client)"));
                    }

                    if (!CheckCertHostname(peerCert.Get(), Location_.Host)) {
                        ythrow TSslException(AsStringBuf("CheckCertHostname(client)"));
                    }
                }
            }

        private:
            const TParsedLocation Location_;
            //TSslSessionHolder Session_;
        };

        static TConnCache* SocketCache() {
            return Singleton<TConnCache>();
        }

        //some templates magic
        template <class T>
        static inline TAutoPtr<T> AutoPtr(T* t) noexcept {
            return t;
        }

        static inline TString ReadAll(THttpInput& in) {
            TString ret;
            ui64 clin;

            if (in.GetContentLength(clin)) {
                const size_t cl = SafeIntegerCast<size_t>(clin);

                ret.ReserveAndResize(cl);
                size_t sz = in.Load(ret.begin(), cl);
                if (sz != cl) {
                    throw yexception() << AsStringBuf("not full content: ") << sz << AsStringBuf(" bytes from ") << cl;
                }
            } else {
                TVector<char> buff(9500); //common jumbo frame size

                while (size_t len = in.Read(~buff, +buff)) {
                    ret.AppendNoAlias(~buff, len);
                }
            }

            return ret;
        }

        template <class TRequestType>
        class THttpsRequest: public IJob {
        public:
            inline THttpsRequest(TSimpleHandleRef hndl, const TMessage& msg)
                : Hndl_(hndl)
                , Msg_(msg)
                , Loc_(msg.Addr)
                , Addr_(CachedThrResolve(TResolveInfo(Loc_.Host, Loc_.GetPort())))
            {
            }

            void DoRun(TCont* c) override {
                THolder<THttpsRequest> This(this);

                if (c->Cancelled()) {
                    Hndl_->NotifyError(new TError("canceled", TError::TType::Cancelled));
                    return;
                }

                TErrorRef error;
                THolder<TConnCache::TConnection> s = SocketCache()->Connect(c, Msg_.Addr, Addr_, &error);
                if (!s) {
                    Hndl_->NotifyError(error);
                    return;
                }

                TString received;
                THttpHeaders headers;

                TSslClientIOStream* io;
                if (s->SslStream() != nullptr) {
                    io = s->SslStream();
                    io->SetCanceledPtr(Hndl_->CanceledPtr());
                } else {
                    io = new TSslClientIOStream(TSslCtxClient::Instance(), Loc_, s->Fd(), Hndl_->CanceledPtr());
                    s->SetSslStream(io);
                }

                try {
                    TContBIOWatcher w(*io, c);
                    if (!io->Initialized())
                        io->Handshake();

                    RequestData().SendTo(*io);
                    Req_.Destroy();
                    error = ProcessRecv(*io, &received, &headers);
                } catch (const TSystemError& e) {
                    if (c->Cancelled() || e.Status() == ECANCELED) {
                        error = new TError("canceled", TError::TType::Cancelled);
                    } else {
                        error = new TError(CurrentExceptionMessage());
                    }
                } catch (...) {
                    if (c->Cancelled()) {
                        error = new TError("canceled", TError::TType::Cancelled);
                    } else {
                        error = new TError(CurrentExceptionMessage());
                    }
                }

                if (error) {
                    Hndl_->NotifyError(error, received);
                } else {
                    SocketCache()->Release(*s);
                    Hndl_->NotifyResponse(received, headers);
                }
            }

            TErrorRef ProcessRecv(TSslClientIOStream& io, TString* data, THttpHeaders* headers) {
                io.WaitUntilWritten();

                Hndl_->SetSendComplete();

                THttpInput in(&io);
                *data = ReadAll(in);
                *headers = in.Headers();

                i32 code = ParseHttpRetCode(in.FirstLine());
                if (code < 200 || code > 299) {
                    return new TError(TStringBuilder() << AsStringBuf("request failed(") << in.FirstLine() << ')', TError::TType::ProtocolSpecific, code);
                }

                return nullptr;
            }

            const NHttp::TRequestData& RequestData() {
                if (!Req_) {
                    Req_ = TRequestType::Build(Msg_, Loc_);
                }
                return *Req_;
            }

        private:
            TSimpleHandleRef Hndl_;
            const TMessage Msg_;
            const TParsedLocation Loc_;
            const TResolvedHost* Addr_;
            NHttp::TRequestData::TPtr Req_;
        };

        class TServer: public IRequester, public TContListener::ICallBack {
            class TSslServerIOStream: public TSslIOStream, public TThrRefBase {
            public:
                TSslServerIOStream(TSslCtxServer& sslCtx, TSocketRef s)
                    : TSslIOStream(sslCtx, new TContBIO(*s))
                    , S_(s)
                {
                }

                void Close(bool shutdown) {
                    if (shutdown) {
                        Shutdown();
                    }
                    S_->Close();
                }

                void Handshake() override {
                    if (!Ssl_) {
                        Ssl_ = SSL_new(SslCtx_);
                        if (THttpsOptions::EnableSslServerDebug) {
                            SSL_set_info_callback(Ssl_.Get(), InfoCB);
                        }

                        THolder<BIO> bio = Connection_->Bio();
                        SSL_set_bio(Ssl_.Get(), bio.Get(), bio.Get());
                        Y_UNUSED(bio.Release());

                        const int rc = SSL_accept(Ssl_.Get());
                        if (1 != rc) {
                            ythrow TSslException(AsStringBuf("SSL_accept"), Ssl_.Get(), rc);
                        }
                    }

                    if (!SSL_is_init_finished(Ssl_.Get())) {
                        const int rc = SSL_do_handshake(Ssl_.Get());
                        if (rc != 1) {
                            ythrow TSslException(AsStringBuf("SSL_do_handshake"), Ssl_.Get(), rc);
                        }
                    }
                }

            private:
                TSocketRef S_;
            };

            class TJobsQueue: public TAutoOneConsumerPipeQueue<IJob>, public TThrRefBase {
            };

            typedef TIntrusivePtr<TJobsQueue> TJobsQueueRef;

            class TWrite: public IJob, public TData {
            private:
                template <class T>
                static void WriteHeader(IOutputStream& os, TStringBuf name, T value) {
                    os << name << AsStringBuf(": ") << value << AsStringBuf("\r\n");
                }

                static void WriteHttpCode(IOutputStream& os, TMaybe<IRequest::TResponseError> error) {
                    if (!error.Defined()) {
                        os << HttpCodeStrEx(HttpCodes::HTTP_OK);
                        return;
                    }

                    switch (*error) {
                        case IRequest::TResponseError::BadRequest:
                            os << HttpCodeStrEx(HttpCodes::HTTP_BAD_REQUEST);
                            break;
                        case IRequest::TResponseError::Forbidden:
                            os << HttpCodeStrEx(HttpCodes::HTTP_FORBIDDEN);
                            break;
                        case IRequest::TResponseError::NotExistService:
                            os << HttpCodeStrEx(HttpCodes::HTTP_NOT_FOUND);
                            break;
                        case IRequest::TResponseError::TooManyRequests:
                            os << HttpCodeStrEx(HttpCodes::HTTP_TOO_MANY_REQUESTS);
                            break;
                        case IRequest::TResponseError::InternalError:
                            os << HttpCodeStrEx(HttpCodes::HTTP_INTERNAL_SERVER_ERROR);
                            break;
                        case IRequest::TResponseError::NotImplemented:
                            os << HttpCodeStrEx(HttpCodes::HTTP_NOT_IMPLEMENTED);
                            break;
                        case IRequest::TResponseError::BadGateway:
                            os << HttpCodeStrEx(HttpCodes::HTTP_BAD_GATEWAY);
                            break;
                        case IRequest::TResponseError::ServiceUnavailable:
                            os << HttpCodeStrEx(HttpCodes::HTTP_SERVICE_UNAVAILABLE);
                            break;
                        case IRequest::TResponseError::BandwidthLimitExceeded:
                            os << HttpCodeStrEx(HttpCodes::HTTP_BANDWIDTH_LIMIT_EXCEEDED);
                            break;
                        case IRequest::TResponseError::MaxResponseError:
                            ythrow yexception() << AsStringBuf("unknow type of error");
                    }
                }

            public:
                inline TWrite(TData& data, const TString& compressionScheme, TIntrusivePtr<TSslServerIOStream> io, TServer* server, const TString& headers)
                    : CompressionScheme_(compressionScheme)
                    , IO_(io)
                    , Server_(server)
                    , Error_(TMaybe<IRequest::TResponseError>())
                    , Headers_(headers)
                {
                    swap(data);
                }

                inline TWrite(TData& data, const TString& compressionScheme, TIntrusivePtr<TSslServerIOStream> io, TServer* server, IRequest::TResponseError error, const TString& headers)
                    : CompressionScheme_(compressionScheme)
                    , IO_(io)
                    , Server_(server)
                    , Error_(error)
                    , Headers_(headers)
                {
                    swap(data);
                }

                void DoRun(TCont* c) override {
                    THolder<TWrite> This(this);

                    try {
                        TContBIOWatcher w(*IO_, c);

                        PrepareSocket(IO_->Socket());

                        char buf[128];
                        TMemoryOutput mo(buf, sizeof(buf));

                        mo << AsStringBuf("HTTP/1.1 ");
                        WriteHttpCode(mo, Error_);
                        mo << AsStringBuf("\r\n");

                        if (!CompressionScheme_.empty()) {
                            WriteHeader(mo, AsStringBuf("Content-Encoding"), TStringBuf(CompressionScheme_));
                        }
                        WriteHeader(mo, AsStringBuf("Connection"), AsStringBuf("Keep-Alive"));
                        WriteHeader(mo, AsStringBuf("Content-Length"), size());

                        mo << Headers_;

                        mo << AsStringBuf("\r\n");

                        IO_->Write(buf, mo.Buf() - buf);
                        if (size()) {
                            IO_->Write(data(), size());
                        }

                        Server_->Enqueue(new TRead(IO_, Server_));
                    } catch (...) {
                    }
                }

            private:
                const TString CompressionScheme_;
                TIntrusivePtr<TSslServerIOStream> IO_;
                TServer* Server_;
                TMaybe<IRequest::TResponseError> Error_;
                TString Headers_;
            };

            class TRequest: public IHttpRequest {
            public:
                inline TRequest(THttpInput& in, TIntrusivePtr<TSslServerIOStream> io, TServer* server)
                    : IO_(io)
                    , Tmp_(in.FirstLine())
                    , CompressionScheme_(in.BestCompressionScheme())
                    , RemoteHost_(PrintHostByRfc(*GetPeerAddr(IO_->Socket())))
                    , Headers_(in.Headers())
                    , H_(Tmp_)
                    , Server_(server)
                {
                }

                ~TRequest() override {
                    if (!!IO_) {
                        try {
                            Server_->Enqueue(new TFail(IO_, Server_));
                        } catch (...) {
                        }
                    }
                }

                TStringBuf Scheme() override {
                    return AsStringBuf("https");
                }

                TString RemoteHost() override {
                    return RemoteHost_;
                }

                const THttpHeaders& Headers() const override {
                    return Headers_;
                }

                TStringBuf Service() override {
                    return TStringBuf(H_.Path).Skip(1);
                }

                TStringBuf RequestId() override {
                    return TStringBuf();
                }

                bool Canceled() override {
                    if (!IO_) {
                        return false;
                    }
                    return !TCont::SocketNotClosedByOtherSide(IO_->Socket());
                }

                void SendReply(TData& data) override {
                    SendReply(data, TString());
                }

                void SendReply(TData& data, const TString& headers) override {
                    const bool compressed = Compress(data);
                    Server_->Enqueue(new TWrite(data, compressed ? CompressionScheme_ : TString(), IO_, Server_, headers));
                    Y_UNUSED(IO_.Release());
                }

                void SendError(TResponseError error, const TString&) override {
                    TData data;
                    Server_->Enqueue(new TWrite(data, TString(), IO_, Server_, error, TString()));
                    Y_UNUSED(IO_.Release());
                }

            private:
                bool Compress(TData& data) const {
                    if (CompressionScheme_ == AsStringBuf("gzip")) {
                        try {
                            TData gzipped(data.size());
                            TMemoryOutput out(~gzipped, +gzipped);
                            TZLibCompress c(&out, ZLib::GZip);
                            c.Write(data.data(), data.size());
                            c.Finish();
                            gzipped.resize(out.Buf() - ~gzipped);
                            data.swap(gzipped);
                            return true;
                        } catch (yexception&) {
                            // gzipped data occupies more space than original data
                        }
                    }
                    return false;
                }

            private:
                TIntrusivePtr<TSslServerIOStream> IO_;
                const TString      Tmp_;
                const TString      CompressionScheme_;
                const TString      RemoteHost_;
                const THttpHeaders Headers_;

            protected:
                TParsedHttpFull H_;
                TServer* Server_;
            };

            class TGetRequest: public TRequest {
            public:
                inline TGetRequest(THttpInput& in, TIntrusivePtr<TSslServerIOStream> io, TServer* server)
                    : TRequest(in, io, server)
                {
                }

                TStringBuf Data() override {
                    return H_.Cgi;
                }
            };

            class TPostRequest: public TRequest {
            public:
                inline TPostRequest(THttpInput& in, TIntrusivePtr<TSslServerIOStream> io, TServer* server)
                    : TRequest(in, io, server)
                    , Data_(ReadAll(in))
                {
                }

                TStringBuf Data() override {
                    return Data_;
                }

            private:
                TString Data_;
            };

            class TFail: public IJob {
            public:
                inline TFail(TIntrusivePtr<TSslServerIOStream> io, TServer* server)
                    : IO_(io)
                    , Server_(server)
                {
                }

                void DoRun(TCont* c) override {
                    THolder<TFail> This(this);
                    const TStringBuf answer = AsStringBuf("HTTP/1.1 503 Service unavailable\r\n"
                                                          "Content-Length: 0\r\n\r\n");

                    try {
                        TContBIOWatcher w(*IO_, c);
                        IO_->Write(answer);
                        Server_->Enqueue(new TRead(IO_, Server_));
                    } catch (...) {
                    }
                }

            private:
                TIntrusivePtr<TSslServerIOStream> IO_;
                TServer* Server_;
            };

            class TRead: public IJob {
            public:
                TRead(TIntrusivePtr<TSslServerIOStream> io, TServer* server, bool selfRemove = false)
                    : IO_(io)
                    , Server_(server)
                    , SelfRemove(selfRemove)
                {
                }

                inline void operator()(TCont* c) {
                    try {
                        TContBIOWatcher w(*IO_, c);

                        if (IO_->PollReadT(TDuration::Seconds(InputConnections()->UnusedConnKeepaliveTimeout()))) {
                            IO_->Close(true);
                            return;
                        }

                        IO_->Handshake();
                        THttpInput in(IO_.Get());

                        const char sym = *~in.FirstLine();

                        if (sym == 'p' || sym == 'P') {
                            Server_->OnRequest(new TPostRequest(in, IO_, Server_));
                        } else {
                            Server_->OnRequest(new TGetRequest(in, IO_, Server_));
                        }
                    } catch (...) {
                        IO_->Close(false);
                    }

                    if (SelfRemove) {
                        delete this;
                    }
                }

            private:
                void DoRun(TCont* c) override {
                    THolder<TRead> This(this);
                    (*this)(c);
                }

            private:
                TIntrusivePtr<TSslServerIOStream> IO_;
                TServer* Server_;
                bool SelfRemove = false;
            };

        public:
            inline TServer(IOnRequest* cb, const TParsedLocation& loc)
                : CB_(cb)
                , E_(RealStackSize(16000))
                , L_(new TContListener(this, &E_, TContListener::TOptions().SetDeferAccept(true)))
                , JQ_(new TJobsQueue())
                , SslCtx_(loc)
            {
                L_->Bind(TNetworkAddress(loc.GetPort()));
                E_.Create<TServer, &TServer::RunDispatcher>(this, "dispatcher");
                Thrs_.push_back(Spawn<TServer, &TServer::Run>(this));
            }

            ~TServer() override {
                JQ_->Enqueue(nullptr);

                for (size_t i = 0; i < +Thrs_; ++i) {
                    Thrs_[i]->Join();
                }
            }

            void Run() {
                //SetHighestThreadPriority();
                L_->Listen();
                E_.Execute();
            }

            inline void OnRequest(const IRequestRef& req) {
                CB_->OnRequest(req);
            }

            TJobsQueueRef& JobQueue() noexcept {
                return JQ_;
            }

            void Enqueue(IJob* j) {
                JQ_->EnqueueSafe(TAutoPtr<IJob>(j));
            }

            void RunDispatcher(TCont* c) {
                for (;;) {
                    TAutoPtr<IJob> job(JQ_->Dequeue(c));

                    if (!job) {
                        break;
                    }

                    try {
                        c->Executor()->Create(*job, "https-job");
                        Y_UNUSED(job.Release());
                    } catch (...) {
                    }
                }

                JQ_->Enqueue(nullptr);
                c->Executor()->Abort();
            }

            void OnAcceptFull(const TAcceptFull& a) override {
                try {
                    TSocketRef s(new TSharedSocket(*a.S));

                    if (InputConnections()->ExceedHardLimit()) {
                        s->Close();
                        return;
                    }

                    THolder<TRead> read(new TRead(new TSslServerIOStream(SslCtx_, s), this, /* selfRemove */ true));
                    E_.Create(*read, "https-response");
                    Y_UNUSED(read.Release());
                    E_.Running()->ContPtr()->Yield();
                } catch (...) {
                }
            }

            void OnError() override {
                try {
                    throw;
                } catch (const TSystemError& e) {
                    //crutch for prevent 100% busyloop (simple suspend listener/accepter)
                    if (e.Status() == EMFILE) {
                        E_.Running()->ContPtr()->SleepT(TDuration::MilliSeconds(500));
                    }
                }
            }

        private:
            IOnRequest* CB_;
            TContExecutor E_;
            THolder<TContListener> L_;
            TVector<TThreadRef> Thrs_;
            TJobsQueueRef JQ_;
            TSslCtxServer SslCtx_;
        };

        template <class T>
        class THttpsProtocol: public IProtocol {
        public:
            IRequesterRef CreateRequester(IOnRequest* cb, const TParsedLocation& loc) override {
                return new TServer(cb, loc);
            }

            THandleRef ScheduleRequest(const TMessage& msg, IOnRecv* fallback, TServiceStatRef& ss) override {
                TSimpleHandleRef ret(new TSimpleHandle(fallback, msg, !ss ? nullptr : new TStatCollector(ss)));
                try {
                    TAutoPtr<THttpsRequest<T>> req(new THttpsRequest<T>(ret, msg));
                    JobQueue()->Schedule(req);
                    return ret.Get();
                } catch (...) {
                    ret->ResetOnRecv();
                    throw;
                }
            }

            TStringBuf Scheme() const noexcept override {
                return T::Name();
            }

            bool SetOption(TStringBuf name, TStringBuf value) override {
                return THttpsOptions::Set(name, value);
            }
        };

        struct TRequestGet: public NHttp::TRequestGet {
            static inline TStringBuf Name() noexcept {
                return AsStringBuf("https");
            }
        };

        struct TRequestFull: public NHttp::TRequestFull {
            static inline TStringBuf Name() noexcept {
                return AsStringBuf("fulls");
            }
        };

        struct TRequestPost: public NHttp::TRequestPost {
            static inline TStringBuf Name() noexcept {
                return AsStringBuf("posts");
            }
        };

    }
}

namespace NNeh {
    IProtocol* SSLGetProtocol() {
        return Singleton<NHttps::THttpsProtocol<NNeh::NHttps::TRequestGet>>();
    }

    IProtocol* SSLPostProtocol() {
        return Singleton<NHttps::THttpsProtocol<NNeh::NHttps::TRequestPost>>();
    }

    IProtocol* SSLFullProtocol() {
        return Singleton<NHttps::THttpsProtocol<NNeh::NHttps::TRequestFull>>();
    }

    void SetHttpOutputConnectionsLimits(size_t softLimit, size_t hardLimit) {
        Y_VERIFY(hardLimit > softLimit, "invalid output fd limits");

        NHttps::SocketCache()->SetFdLimits(softLimit, hardLimit);
    }

    void SetHttpInputConnectionsLimits(size_t softLimit, size_t hardLimit) {
        Y_VERIFY(hardLimit > softLimit, "invalid input fd limits");

        NHttps::InputConnections()->SetFdLimits(softLimit, hardLimit);
    }

    void SetHttpInputConnectionsTimeouts(unsigned minSec, unsigned maxSec) {
        Y_VERIFY(maxSec > minSec, "invalid input fd limits timeouts");

        NHttps::InputConnections()->MinUnusedConnKeepaliveTimeout = minSec;
        NHttps::InputConnections()->MaxUnusedConnKeepaliveTimeout = maxSec;
    }
}
