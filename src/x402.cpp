#include "exchange/x402.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

namespace exchange {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t max_http_body_size = 1024 * 1024;
        constexpr std::size_t max_http_request_header_size = 8 * 1024;
        constexpr std::string_view payment_required_header =
            "payment-required:";

        class CurlGlobalState {
        public:
            CurlGlobalState() {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                    throw X402TransportError(
                        "failed to initialize x402 HTTP transport");
                }
            }

            ~CurlGlobalState() {
                curl_global_cleanup();
            }
        };

        class FileDescriptor {
        public:
            explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
            ~FileDescriptor() noexcept {
                if (fd_ >= 0) {
                    static_cast<void>(::close(fd_));
                }
            }

            FileDescriptor(const FileDescriptor&) = delete;
            FileDescriptor& operator=(const FileDescriptor&) = delete;

            int get() const noexcept {
                return fd_;
            }

        private:
            int fd_;
        };

        struct HttpResponse {
            long status{};
            std::string body;
            std::string payment_required;
            std::size_t payment_required_count{};
            bool exceeded_limit{};
        };

        CurlGlobalState& curl_global_state() {
            static CurlGlobalState state;
            return state;
        }

        std::string_view trim(std::string_view value) {
            while (!value.empty()
                   && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty()
                   && (value.back() == '\r' || value.back() == '\n'
                       || value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            return value;
        }

        bool starts_with_case_insensitive(
            std::string_view value,
            std::string_view prefix) {
            if (value.size() < prefix.size()) {
                return false;
            }
            for (std::size_t index = 0; index < prefix.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(value[index]))
                    != std::tolower(
                        static_cast<unsigned char>(prefix[index]))) {
                    return false;
                }
            }
            return true;
        }

        std::size_t append_body(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > std::numeric_limits<std::size_t>::max()
                    / size) {
                return 0;
            }
            const std::size_t bytes = size * count;
            auto& response = *static_cast<HttpResponse*>(context);
            if (bytes > max_http_body_size - response.body.size()) {
                response.exceeded_limit = true;
                return 0;
            }
            try {
                response.body.append(data, bytes);
                return bytes;
            } catch (...) {
                return 0;
            }
        }

        std::size_t capture_header(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > std::numeric_limits<std::size_t>::max()
                    / size) {
                return 0;
            }
            const std::size_t bytes = size * count;
            auto& response = *static_cast<HttpResponse*>(context);
            try {
                const std::string_view line(data, bytes);
                if (starts_with_case_insensitive(
                        line, payment_required_header)) {
                    ++response.payment_required_count;
                    response.payment_required = std::string(trim(
                        line.substr(payment_required_header.size())));
                }
                return bytes;
            } catch (...) {
                return 0;
            }
        }

        void require_localhost_url(std::string_view url) {
            constexpr std::string_view prefix = "http://127.0.0.1:";
            if (!url.starts_with(prefix)) {
                throw std::invalid_argument(
                    "x402 v0 only permits http://127.0.0.1 URLs");
            }
            const std::size_t path = url.find('/', prefix.size());
            const std::string_view port = url.substr(
                prefix.size(),
                (path == std::string_view::npos ? url.size() : path)
                    - prefix.size());
            if (port.empty()
                || !std::all_of(port.begin(), port.end(), [](char value) {
                    return value >= '0' && value <= '9';
                })) {
                throw std::invalid_argument(
                    "x402 localhost URL has an invalid port");
            }
            unsigned long parsed_port = 0;
            for (char character : port) {
                parsed_port = parsed_port * 10
                    + static_cast<unsigned long>(character - '0');
                if (parsed_port > 65'535) {
                    break;
                }
            }
            if (parsed_port == 0 || parsed_port > 65'535) {
                throw std::invalid_argument(
                    "x402 localhost URL has an invalid port");
            }
        }

        std::string encode_base64(std::string_view input) {
            constexpr std::string_view alphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string output;
            output.reserve(((input.size() + 2) / 3) * 4);
            for (std::size_t index = 0; index < input.size(); index += 3) {
                const std::uint32_t first = static_cast<unsigned char>(
                    input[index]);
                const bool has_second = index + 1 < input.size();
                const bool has_third = index + 2 < input.size();
                const std::uint32_t second = has_second
                    ? static_cast<unsigned char>(input[index + 1])
                    : 0;
                const std::uint32_t third = has_third
                    ? static_cast<unsigned char>(input[index + 2])
                    : 0;
                const std::uint32_t value =
                    (first << 16) | (second << 8) | third;
                output.push_back(alphabet[(value >> 18) & 0x3f]);
                output.push_back(alphabet[(value >> 12) & 0x3f]);
                output.push_back(
                    has_second ? alphabet[(value >> 6) & 0x3f] : '=');
                output.push_back(has_third ? alphabet[value & 0x3f] : '=');
            }
            return output;
        }

        int base64_value(char character) {
            if (character >= 'A' && character <= 'Z') {
                return character - 'A';
            }
            if (character >= 'a' && character <= 'z') {
                return character - 'a' + 26;
            }
            if (character >= '0' && character <= '9') {
                return character - '0' + 52;
            }
            if (character == '+') {
                return 62;
            }
            if (character == '/') {
                return 63;
            }
            return -1;
        }

        std::string decode_base64(std::string_view input) {
            if (input.empty() || input.size() % 4 != 0) {
                throw X402ProtocolError(
                    "PAYMENT-REQUIRED is not valid Base64");
            }
            std::string output;
            output.reserve((input.size() / 4) * 3);
            for (std::size_t index = 0; index < input.size(); index += 4) {
                const bool final_group = index + 4 == input.size();
                const bool second_padding = input[index + 2] == '=';
                const bool third_padding = input[index + 3] == '=';
                if ((!final_group && (second_padding || third_padding))
                    || (second_padding && !third_padding)) {
                    throw X402ProtocolError(
                        "PAYMENT-REQUIRED has invalid Base64 padding");
                }
                const int first = base64_value(input[index]);
                const int second = base64_value(input[index + 1]);
                const int third = second_padding
                    ? 0
                    : base64_value(input[index + 2]);
                const int fourth = third_padding
                    ? 0
                    : base64_value(input[index + 3]);
                if (first < 0 || second < 0 || third < 0 || fourth < 0) {
                    throw X402ProtocolError(
                        "PAYMENT-REQUIRED is not valid Base64");
                }
                const std::uint32_t value =
                    (static_cast<std::uint32_t>(first) << 18)
                    | (static_cast<std::uint32_t>(second) << 12)
                    | (static_cast<std::uint32_t>(third) << 6)
                    | static_cast<std::uint32_t>(fourth);
                output.push_back(static_cast<char>((value >> 16) & 0xff));
                if (!second_padding) {
                    output.push_back(static_cast<char>((value >> 8) & 0xff));
                }
                if (!third_padding) {
                    output.push_back(static_cast<char>(value & 0xff));
                }
            }
            return output;
        }

        const std::string& require_string(
            const Json& object,
            const char* field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_string()
                || value->get_ref<const std::string&>().empty()) {
                throw X402ProtocolError(
                    std::string("x402 requirement has invalid ") + field);
            }
            return value->get_ref<const std::string&>();
        }

        void validate_atomic_amount(std::string_view amount) {
            if (amount.empty()
                || !std::all_of(
                    amount.begin(), amount.end(), [](char character) {
                        return character >= '0' && character <= '9';
                    })
                || std::all_of(
                    amount.begin(), amount.end(), [](char character) {
                        return character == '0';
                    })) {
                throw std::invalid_argument(
                    "x402 amount must be a positive atomic-unit string");
            }
        }

        ExternalPaymentRequirement parse_requirement(
            std::string_view encoded) {
            try {
                const Json envelope = Json::parse(decode_base64(encoded));
                if (!envelope.is_object()) {
                    throw X402ProtocolError(
                        "x402 PaymentRequired is not an object");
                }
                const auto version = envelope.find("x402Version");
                if (version == envelope.end()
                    || !version->is_number_integer()
                    || version->get<std::int64_t>() != 2) {
                    throw X402ProtocolError(
                        "x402 PaymentRequired version must be 2");
                }
                const auto resource = envelope.find("resource");
                if (resource == envelope.end() || !resource->is_object()) {
                    throw X402ProtocolError(
                        "x402 PaymentRequired has no resource object");
                }
                const auto accepts = envelope.find("accepts");
                if (accepts == envelope.end() || !accepts->is_array()
                    || accepts->size() != 1
                    || !(*accepts)[0].is_object()) {
                    throw X402ProtocolError(
                        "x402 v0 requires exactly one payment option");
                }
                const Json& accepted = (*accepts)[0];
                const auto timeout = accepted.find("maxTimeoutSeconds");
                if (timeout == accepted.end()
                    || !timeout->is_number_integer()) {
                    throw X402ProtocolError(
                        "x402 requirement has invalid maxTimeoutSeconds");
                }
                const std::int64_t timeout_value =
                    timeout->get<std::int64_t>();
                if (timeout_value <= 0
                    || timeout_value
                        > static_cast<std::int64_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
                    throw X402ProtocolError(
                        "x402 requirement has invalid maxTimeoutSeconds");
                }

                ExternalPaymentRequirement requirement{
                    2,
                    require_string(*resource, "url"),
                    require_string(*resource, "description"),
                    require_string(*resource, "mimeType"),
                    require_string(accepted, "scheme"),
                    require_string(accepted, "network"),
                    require_string(accepted, "amount"),
                    require_string(accepted, "asset"),
                    require_string(accepted, "payTo"),
                    static_cast<std::uint32_t>(timeout_value),
                };
                try {
                    validate_atomic_amount(requirement.amount);
                } catch (const std::invalid_argument&) {
                    throw X402ProtocolError(
                        "x402 requirement has invalid atomic amount");
                }
                return requirement;
            } catch (const X402ProtocolError&) {
                throw;
            } catch (const Json::exception&) {
                throw X402ProtocolError(
                    "PAYMENT-REQUIRED contains malformed JSON");
            }
        }

        std::string make_payment_required(
            const X402PaidServiceConfig& config,
            std::string_view resource_url) {
            const Json envelope = {
                {"x402Version", 2},
                {"resource",
                 {{"url", resource_url},
                  {"description", config.description},
                  {"mimeType", config.mime_type}}},
                {"accepts",
                 {{{"scheme", config.scheme},
                   {"network", config.network},
                   {"amount", config.amount},
                   {"asset", config.asset},
                   {"payTo", config.pay_to},
                   {"maxTimeoutSeconds", config.max_timeout_seconds}}}},
            };
            return encode_base64(envelope.dump());
        }

        void validate_service_config(const X402PaidServiceConfig& config) {
            if (config.resource_path.empty()
                || config.resource_path.front() != '/'
                || config.resource_path.find_first_of(" \r\n\t")
                    != std::string::npos) {
                throw std::invalid_argument(
                    "x402 resource path must be an absolute HTTP path");
            }
            for (const auto* value : {
                     &config.description,
                     &config.mime_type,
                     &config.scheme,
                     &config.network,
                     &config.asset,
                     &config.pay_to}) {
                if (value->empty()) {
                    throw std::invalid_argument(
                        "x402 service fields must be non-empty");
                }
            }
            if (config.network.find(':') == std::string::npos) {
                throw std::invalid_argument(
                    "x402 network must use a CAIP-2 identifier");
            }
            validate_atomic_amount(config.amount);
            if (config.max_timeout_seconds == 0) {
                throw std::invalid_argument(
                    "x402 max timeout must be positive");
            }
        }

        void send_all(int fd, std::string_view response) {
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t result = ::send(
                    fd,
                    response.data() + sent,
                    response.size() - sent,
                    MSG_NOSIGNAL);
                if (result > 0) {
                    sent += static_cast<std::size_t>(result);
                    continue;
                }
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                throw X402TransportError(
                    std::string("x402 service send failed: ")
                    + std::strerror(errno));
            }
        }

        std::string receive_request_header(int fd) {
            std::string request;
            std::array<char, 2048> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(
                    fd, buffer.data(), buffer.size(), 0);
                if (received > 0) {
                    if (static_cast<std::size_t>(received)
                        > max_http_request_header_size - request.size()) {
                        throw X402ProtocolError(
                            "x402 service request header is too large");
                    }
                    request.append(
                        buffer.data(), static_cast<std::size_t>(received));
                    continue;
                }
                if (received < 0 && errno == EINTR) {
                    continue;
                }
                if (received == 0) {
                    throw X402ProtocolError(
                        "x402 service received an incomplete request");
                }
                throw X402TransportError(
                    std::string("x402 service recv failed: ")
                    + std::strerror(errno));
            }
            return request;
        }

        std::string request_path(std::string_view request) {
            const std::size_t line_end = request.find("\r\n");
            if (line_end == std::string_view::npos) {
                return {};
            }
            const std::string_view line = request.substr(0, line_end);
            constexpr std::string_view method = "GET ";
            if (!line.starts_with(method)) {
                return {};
            }
            const std::size_t separator = line.find(' ', method.size());
            if (separator == std::string_view::npos) {
                return {};
            }
            const std::string_view version = line.substr(separator + 1);
            if (version != "HTTP/1.1" && version != "HTTP/1.0") {
                return {};
            }
            return std::string(line.substr(
                method.size(), separator - method.size()));
        }
    }  // namespace

    CurlX402Client::CurlX402Client(CurlX402ClientConfig config)
        : config_(config) {
        if (config_.timeout.count() <= 0
            || config_.timeout.count() > LONG_MAX) {
            throw std::invalid_argument(
                "x402 HTTP timeout must be positive and in range");
        }
        static_cast<void>(curl_global_state());
    }

    X402RequestResult CurlX402Client::get(std::string_view url) {
        require_localhost_url(url);
        using CurlHandle =
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
        if (!handle) {
            throw X402TransportError("failed to create x402 HTTP request");
        }

        const std::string owned_url(url);
        HttpResponse response;
        char error_buffer[CURL_ERROR_SIZE]{};
        const auto set_option = [&handle](CURLoption option, auto value) {
            if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) {
                throw X402TransportError(
                    "failed to configure x402 HTTP request");
            }
        };
        set_option(CURLOPT_URL, owned_url.c_str());
        set_option(CURLOPT_HTTPGET, 1L);
        set_option(CURLOPT_FOLLOWLOCATION, 0L);
        set_option(CURLOPT_NOPROXY, "*");
        set_option(CURLOPT_WRITEFUNCTION, &append_body);
        set_option(CURLOPT_WRITEDATA, &response);
        set_option(CURLOPT_HEADERFUNCTION, &capture_header);
        set_option(CURLOPT_HEADERDATA, &response);
        set_option(
            CURLOPT_TIMEOUT_MS,
            static_cast<long>(config_.timeout.count()));
        set_option(
            CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(config_.timeout.count()));
        set_option(CURLOPT_NOSIGNAL, 1L);
        set_option(CURLOPT_ERRORBUFFER, error_buffer);

        const CURLcode result = curl_easy_perform(handle.get());
        if (result != CURLE_OK) {
            if (response.exceeded_limit) {
                throw X402TransportError(
                    "x402 HTTP response exceeded size limit");
            }
            const char* detail = error_buffer[0] == '\0'
                ? curl_easy_strerror(result)
                : error_buffer;
            throw X402TransportError(
                std::string("x402 HTTP transport failure: ") + detail);
        }
        if (curl_easy_getinfo(
                handle.get(), CURLINFO_RESPONSE_CODE, &response.status)
            != CURLE_OK) {
            throw X402TransportError("failed to read x402 HTTP status");
        }

        X402RequestResult request_result{
            static_cast<int>(response.status), std::nullopt, std::nullopt};
        if (response.status == 402) {
            if (response.payment_required_count != 1
                || response.payment_required.empty()) {
                throw X402ProtocolError(
                    "HTTP 402 must contain exactly one PAYMENT-REQUIRED header");
            }
            request_result.payment_required = parse_requirement(
                response.payment_required);
            return request_result;
        }
        if (!response.body.empty()) {
            request_result.resource_body = std::move(response.body);
        }
        return request_result;
    }

    X402PaidService::X402PaidService(
        X402PaidServiceConfig config,
        std::uint16_t port)
        : config_(std::move(config)) {
        validate_service_config(config_);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) {
            throw X402TransportError(
                std::string("x402 service socket failed: ")
                + std::strerror(errno));
        }
        try {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(
                    listen_fd_,
                    reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) < 0) {
                throw X402TransportError(
                    std::string("x402 service bind failed: ")
                    + std::strerror(errno));
            }
            if (::listen(listen_fd_, 1) < 0) {
                throw X402TransportError(
                    std::string("x402 service listen failed: ")
                    + std::strerror(errno));
            }
            socklen_t address_length = sizeof(address);
            if (::getsockname(
                    listen_fd_,
                    reinterpret_cast<sockaddr*>(&address),
                    &address_length) < 0) {
                throw X402TransportError(
                    std::string("x402 service getsockname failed: ")
                    + std::strerror(errno));
            }
            local_port_ = ntohs(address.sin_port);
        } catch (...) {
            static_cast<void>(::close(listen_fd_));
            listen_fd_ = -1;
            throw;
        }
    }

    X402PaidService::~X402PaidService() noexcept {
        if (listen_fd_ >= 0) {
            static_cast<void>(::close(listen_fd_));
        }
    }

    std::uint16_t X402PaidService::local_port() const noexcept {
        return local_port_;
    }

    std::string X402PaidService::resource_url() const {
        return "http://127.0.0.1:" + std::to_string(local_port_)
            + config_.resource_path;
    }

    void X402PaidService::serve_once() {
        int client_fd;
        do {
            client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        } while (client_fd < 0 && errno == EINTR);
        if (client_fd < 0) {
            throw X402TransportError(
                std::string("x402 service accept failed: ")
                + std::strerror(errno));
        }
        const FileDescriptor client(client_fd);
        const std::string request = receive_request_header(client.get());
        if (request_path(request) != config_.resource_path) {
            send_all(
                client.get(),
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 10\r\n"
                "Connection: close\r\n\r\n"
                "Not Found\n");
            return;
        }

        const std::string encoded = make_payment_required(
            config_, resource_url());
        const std::string response =
            "HTTP/1.1 402 Payment Required\r\n"
            "PAYMENT-REQUIRED: " + encoded + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(client.get(), response);
    }
}  // namespace exchange
