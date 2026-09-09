#include "fixture.hpp"
#include <arpa/inet.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
std::atomic<unsigned> calls{};
thread_local bool fail_thread{};
} // namespace
namespace fixture {
auto pause_native_tls() -> void;
}
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                     void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attrs,
                                     void* (*start)(void*), void* argument) {
  if (std::exchange(fail_thread, false)) return EAGAIN;
  return __real_pthread_create(thread, attrs, start, argument);
}
extern "C" SSL_CTX* __real_SSL_CTX_new(const SSL_METHOD*);
extern "C" SSL_CTX* __wrap_SSL_CTX_new(const SSL_METHOD* method) {
  ++calls;
  fixture::pause_native_tls();
  return __real_SSL_CTX_new(method);
}
extern "C" X509* __real_PEM_read_bio_X509(BIO*, X509**, pem_password_cb*,
                                          void*);
extern "C" X509* __wrap_PEM_read_bio_X509(BIO* bio, X509** out,
                                          pem_password_cb* cb, void* data) {
  ++calls;
  return __real_PEM_read_bio_X509(bio, out, cb, data);
}
extern "C" EVP_PKEY* __real_PEM_read_bio_PrivateKey(BIO*, EVP_PKEY**,
                                                    pem_password_cb*, void*);
extern "C" EVP_PKEY* __wrap_PEM_read_bio_PrivateKey(BIO* bio, EVP_PKEY** out,
                                                    pem_password_cb* cb,
                                                    void* data) {
  ++calls;
  return __real_PEM_read_bio_PrivateKey(bio, out, cb, data);
}
namespace fixture {
struct TlsPause::Impl {
  std::mutex mutex;
  std::condition_variable ready;
  std::atomic<bool> entered{};
  std::atomic<bool> released{};
  static std::mutex pending_mutex;
  static std::shared_ptr<Impl> pending;
};
std::mutex TlsPause::Impl::pending_mutex;
std::shared_ptr<TlsPause::Impl> TlsPause::Impl::pending;
TlsPause::TlsPause() : m_impl(std::make_shared<Impl>()) {
  const std::lock_guard lock{Impl::pending_mutex};
  Impl::pending = m_impl;
}
TlsPause::~TlsPause() {
  {
    const std::lock_guard lock{Impl::pending_mutex};
    if (Impl::pending == m_impl) Impl::pending.reset();
  }
  release();
}
auto TlsPause::release() noexcept -> void {
  m_impl->released.store(true);
  m_impl->ready.notify_all();
}
auto TlsPause::entered() const noexcept -> bool {
  return m_impl->entered.load();
}
auto pause_native_tls() -> void {
  const auto state = [] {
    const std::lock_guard lock{TlsPause::Impl::pending_mutex};
    return std::exchange(TlsPause::Impl::pending, nullptr);
  }();
  if (!state) return;
  state->entered.store(true);
  std::unique_lock lock{state->mutex};
  while (!state->released.load())
    state->ready.wait_for(lock, std::chrono::milliseconds{2});
}
auto fail_next_thread_start(bool enabled) noexcept -> void {
  fail_thread = enabled;
}
namespace {
using namespace aiforge;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
auto ensure(bool valid) -> void {
  if (!valid) throw std::runtime_error("TLS fixture setup failed");
}
auto key() -> Key {
  Key value{EVP_EC_gen("prime256v1"), EVP_PKEY_free};
  ensure(bool(value));
  return value;
}
auto extension(X509* cert, X509* issuer, int nid, const char* text) -> void {
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> value{
      X509V3_EXT_conf_nid(nullptr, &context, nid, text), X509_EXTENSION_free};
  ensure(value && X509_add_ext(cert, value.get(), -1) == 1);
}
auto certificate(EVP_PKEY* public_key, std::string_view name, X509* issuer,
                 EVP_PKEY* signing_key, bool ca, const char* san = nullptr,
                 bool expired = false) -> Cert {
  static std::atomic<long> serial{1};
  Cert cert{X509_new(), X509_free};
  ensure(bool(cert));
  ensure(X509_set_version(cert.get(), 2) == 1 &&
         ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial++) == 1);
  ensure(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -86400) != nullptr);
  ensure(X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                         expired ? -3600 : 86400) != nullptr);
  ensure(X509_set_pubkey(cert.get(), public_key) == 1);
  auto* subject = X509_get_subject_name(cert.get());
  ensure(X509_NAME_add_entry_by_txt(
             subject, "CN", MBSTRING_ASC,
             reinterpret_cast<const unsigned char*>(name.data()),
             static_cast<int>(name.size()), -1, 0) == 1);
  ensure(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer)
                                                 : subject) == 1);
  extension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
            ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  extension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
            ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
  if (san) extension(cert.get(), issuer, NID_subject_alt_name, san);
  ensure(X509_sign(cert.get(), signing_key, EVP_sha256()) > 0);
  return cert;
}
auto pem(X509* cert) -> std::string {
  Bio buffer{BIO_new(BIO_s_mem()), BIO_free};
  ensure(bool(buffer));
  ensure(PEM_write_bio_X509(buffer.get(), cert) == 1);
  char* data{};
  const auto size = BIO_get_mem_data(buffer.get(), &data);
  ensure(size > 0);
  return {data, static_cast<std::size_t>(size)};
}
auto pem(EVP_PKEY* key_value) -> std::string {
  Bio buffer{BIO_new(BIO_s_mem()), BIO_free};
  ensure(bool(buffer));
  ensure(PEM_write_bio_PrivateKey(buffer.get(), key_value, nullptr, nullptr, 0,
                                  nullptr, nullptr) == 1);
  char* data{};
  const auto size = BIO_get_mem_data(buffer.get(), &data);
  ensure(size > 0);
  return {data, static_cast<std::size_t>(size)};
}
auto material() -> Credentials {
  auto server_root_key = key();
  auto client_root_key = key();
  auto intermediate_key = key();
  auto server_key = key();
  auto client_key = key();
  auto other_key = key();
  auto root = certificate(server_root_key.get(), "server-root", nullptr,
                          server_root_key.get(), true);
  auto client_root = certificate(client_root_key.get(), "client-root", nullptr,
                                 client_root_key.get(), true);
  auto intermediate =
      certificate(intermediate_key.get(), "client-intermediate",
                  client_root.get(), client_root_key.get(), true);
  auto server =
      certificate(server_key.get(), "server", root.get(), server_root_key.get(),
                  false, "DNS:localhost,IP:127.0.0.1,IP:::1");
  auto wrong = certificate(server_key.get(), "wrong", root.get(),
                           server_root_key.get(), false, "DNS:wrong.invalid");
  auto expired =
      certificate(server_key.get(), "expired", root.get(),
                  server_root_key.get(), false, "IP:127.0.0.1", true);
  auto client = certificate(client_key.get(), "client", intermediate.get(),
                            intermediate_key.get(), false);
  return {pem(root.get()),        pem(server.get()),
          pem(server_key.get()),  pem(wrong.get()),
          pem(expired.get()),     pem(client_root.get()),
          pem(client_root.get()), pem(client.get()) + pem(intermediate.get()),
          pem(client.get()),      pem(client_key.get()),
          pem(other_key.get())};
}
auto b64(std::string_view input) -> std::string {
  std::string out(4 * ((input.size() + 2) / 3), '\0');
  ensure(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                         reinterpret_cast<const unsigned char*>(input.data()),
                         static_cast<int>(input.size())) ==
         static_cast<int>(out.size()));
  return out;
}
template <class T> auto id(std::string text) -> T {
  return T::from(std::move(text)).value();
}
class Descriptor {
 public:
  explicit Descriptor(int descriptor = -1) : fd(descriptor) {}
  ~Descriptor() {
    if (fd >= 0) (void)::close(fd);
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  int fd;
};
} // namespace
auto native_calls() noexcept -> unsigned {
  return calls.load();
}
auto credentials() -> const Credentials& {
  static const auto value = material();
  return value;
}
struct Peer::Impl {
  PeerOptions options;
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr,
                                                            SSL_CTX_free};
  Descriptor listener;
  std::uint16_t bound_port{};
  std::atomic<unsigned> accepts{};
  std::atomic<bool> request_seen{};
  std::atomic<bool> auth{};
  mutable std::mutex mutex;
  std::string line;
  std::jthread thread;
  explicit Impl(PeerOptions settings) : options(std::move(settings)) {
    const auto& certs = credentials();
    context.reset(SSL_CTX_new(TLS_server_method()));
    ensure(bool(context));
    const auto& certificate_text = options.wrong_host ? certs.wrong_host_cert
                                   : options.expired  ? certs.expired_cert
                                                      : certs.server_cert;
    Bio cert_bio{BIO_new_mem_buf(certificate_text.data(),
                                 static_cast<int>(certificate_text.size())),
                 BIO_free};
    Bio key_bio{BIO_new_mem_buf(certs.server_key.data(),
                                static_cast<int>(certs.server_key.size())),
                BIO_free};
    Cert certificate_value{
        PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr),
        X509_free};
    Key key_value{
        PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr),
        EVP_PKEY_free};
    ensure(certificate_value && key_value &&
           SSL_CTX_use_certificate(context.get(), certificate_value.get()) ==
               1 &&
           SSL_CTX_use_PrivateKey(context.get(), key_value.get()) == 1);
    if (options.require_client) {
      Bio root_bio{BIO_new_mem_buf(certs.client_ca.data(),
                                   static_cast<int>(certs.client_ca.size())),
                   BIO_free};
      Cert root{PEM_read_bio_X509(root_bio.get(), nullptr, nullptr, nullptr),
                X509_free};
      ensure(root && X509_STORE_add_cert(SSL_CTX_get_cert_store(context.get()),
                                         root.get()) == 1);
      SSL_CTX_set_verify(context.get(),
                         SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                         nullptr);
    }
    listener.fd =
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    ensure(listener.fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ensure(::bind(listener.fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    ensure(::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address),
                         &length) == 0);
    bound_port = ntohs(address.sin_port);
    ensure(::listen(listener.fd, 4) == 0);
    thread = std::jthread{[this](std::stop_token stop) { run(stop); }};
  }
  ~Impl() {
    thread.request_stop();
    if (thread.joinable()) thread.join();
  }
  auto wait(int fd, short events, std::stop_token stop,
            std::chrono::steady_clock::time_point deadline) -> bool {
    while (!stop.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{fd, events, 0};
      const auto result = ::poll(&descriptor, 1, 10);
      if (result > 0) return (descriptor.revents & events) != 0;
      if (result < 0 && errno != EINTR) return false;
    }
    return false;
  }
  auto step(SSL* ssl, int result, std::stop_token stop,
            std::chrono::steady_clock::time_point deadline) -> bool {
    const auto error = SSL_get_error(ssl, result);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
      return false;
    return wait(SSL_get_fd(ssl),
                error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, stop,
                deadline);
  }
  auto serve(std::stop_token stop,
             std::chrono::steady_clock::time_point deadline) -> void {
    if (!wait(listener.fd, POLLIN, stop, deadline)) return;
    Descriptor socket{
        ::accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
    if (socket.fd < 0) return;
    ++accepts;
    if (options.stall_handshake) {
      while (!stop.stop_requested() &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
      return;
    }
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl{SSL_new(context.get()),
                                                  SSL_free};
    if (!ssl || SSL_set_fd(ssl.get(), socket.fd) != 1) return;
    for (;;) {
      const auto result = SSL_accept(ssl.get());
      if (result == 1) break;
      if (!step(ssl.get(), result, stop, deadline)) return;
    }
    std::string request;
    std::array<char, 1024> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < 16384) {
      const auto read =
          SSL_read(ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
      if (read > 0)
        request.append(buffer.data(), static_cast<std::size_t>(read));
      else if (!step(ssl.get(), read, stop, deadline))
        return;
    }
    {
      std::lock_guard lock{mutex};
      line = request.substr(0, request.find("\r\n"));
    }
    auth.store(request.find("Authorization: Bearer " + std::string{token} +
                            "\r\n") != std::string::npos ||
               (options.require_client &&
                SSL_get_verify_result(ssl.get()) == X509_V_OK));
    request_seen.store(true);
    std::string_view output = options.response;
    if (options.stall_body)
      output = output.substr(0, output.find("\r\n\r\n") + 4);
    while (!output.empty() && !stop.stop_requested()) {
      const auto wrote =
          SSL_write(ssl.get(), output.data(), static_cast<int>(output.size()));
      if (wrote > 0)
        output.remove_prefix(static_cast<std::size_t>(wrote));
      else if (!step(ssl.get(), wrote, stop, deadline))
        return;
    }
    if (options.stall_body)
      while (!stop.stop_requested() &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    (void)SSL_shutdown(ssl.get());
  }
  auto run(std::stop_token stop) noexcept -> void {
    try {
      sigset_t mask{};
      sigemptyset(&mask);
      sigaddset(&mask, SIGPIPE);
      (void)pthread_sigmask(SIG_BLOCK, &mask, nullptr);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{4};
      for (unsigned i = 0; i < options.connections && !stop.stop_requested();
           ++i)
        serve(stop, deadline);
    } catch (
        ...) { /* Fixed fixture state only; never leak request/TLS bytes. */
    }
  }
};
Peer::Peer(PeerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options))) {
}
Peer::~Peer() = default;
auto Peer::port() const noexcept -> std::uint16_t {
  return m_impl->bound_port;
}
auto Peer::accepted() const noexcept -> unsigned {
  return m_impl->accepts.load();
}
auto Peer::received() const noexcept -> bool {
  return m_impl->request_seen.load();
}
auto Peer::authorized() const noexcept -> bool {
  return m_impl->auth.load();
}
auto Peer::request_line() const -> std::string {
  std::lock_guard lock{m_impl->mutex};
  return m_impl->line;
}
auto configuration(std::uint16_t port, bool certificate_auth, std::string ca,
                   std::string chain, std::string private_key, std::string host)
    -> adapters::StaticKubernetesConfig {
  const auto& certs = credentials();
  if (ca.empty()) ca = certs.server_ca;
  if (chain.empty()) chain = certs.client_chain;
  if (private_key.empty()) private_key = certs.client_key;
  nlohmann::json user =
      certificate_auth ? nlohmann::json{{"client-certificate-data", b64(chain)},
                                        {"client-key-data", b64(private_key)}}
                       : nlohmann::json{{"token", token}};
  nlohmann::json value{
      {"apiVersion", "v1"},
      {"kind", "Config"},
      {"clusters",
       {{{"name", "cluster"},
         {"cluster",
          {{"server", "https://" + host + ":" + std::to_string(port)},
           {"certificate-authority-data", b64(ca)}}}}}},
      {"users", {{{"name", "user"}, {"user", std::move(user)}}}},
      {"contexts",
       {{{"name", "context"},
         {"context",
          {{"cluster", "cluster"},
           {"user", "user"},
           {"namespace", "selected"}}}}}}};
  return adapters::StaticKubernetesConfig::parse(
             value.dump(), adapters::StaticKubernetesSyntax::json, "context",
             "selected")
      .value();
}
auto source(adapters::StaticKubernetesConfig configuration_value)
    -> std::shared_ptr<Source> {
  domain::OpsTargetBinding binding{
      id<domain::OpsTargetId>("target"),
      id<domain::OpsConfigurationRevision>("revision"),
      configuration_value.identity()};
  return Source::create(binding, std::move(configuration_value)).value();
}
auto request(const Source& source_value, Operation operation)
    -> domain::OpsObservationRequest {
  domain::OpsObservationRequest result{id<domain::OpsOwnerId>("owner"),
                                       id<domain::SessionId>("session"),
                                       id<domain::OpsRequestId>("request"),
                                       source_value.target_binding(),
                                       1,
                                       operation,
                                       {},
                                       1,
                                       {}};
  result.limits.timeout = std::chrono::milliseconds{1500};
  if (operation == Operation::kubernetes_pod_health)
    result.resource = domain::KubernetesPodIdentity{
        "selected", "pod-one", id<domain::OpsResourceUid>("pod-uid"), {}};
  return result;
}
auto pod() -> std::string {
  return R"({"apiVersion":"v1","kind":"Pod","metadata":{"namespace":"selected","name":"pod-one","uid":"pod-uid","resourceVersion":"rv-one"},"spec":{"containers":[{"name":"main"}]},"status":{"phase":"Running","containerStatuses":[{"name":"main","containerID":"containerd://one","ready":true,"state":{"running":{}}}]}})";
}
auto list(bool events) -> std::string {
  if (events)
    return R"({"apiVersion":"v1","kind":"EventList","metadata":{"resourceVersion":"events-rv"},"items":[]})";
  return R"({"apiVersion":"v1","kind":"PodList","metadata":{"resourceVersion":"list-rv"},"items":[)" +
         pod() + "]}";
}
auto response(std::string body, std::string headers, int status)
    -> std::string {
  return "HTTP/1.1 " + std::to_string(status) +
         " Result\r\nContent-Type: application/json\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\n" + headers +
         "Connection: close\r\n\r\n" + body;
}
} // namespace fixture
