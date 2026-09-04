#include "exchange/binance_agentic_wallet.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

extern char** environ;

namespace exchange {
    namespace {
        using Json = nlohmann::json;

        constexpr std::size_t max_process_output = 1024 * 1024;

        struct ProcessResult {
            int exit_code{};
            std::string standard_output;
            std::string standard_error;
        };

        class Pipe {
        public:
            Pipe() {
                if (::pipe2(fds_.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
                    throw BinanceAgenticWalletTransportError(
                        std::string("failed to create baw output pipe: ")
                        + std::strerror(errno));
                }
            }

            ~Pipe() noexcept {
                close_read();
                close_write();
            }

            Pipe(const Pipe&) = delete;
            Pipe& operator=(const Pipe&) = delete;

            int read_fd() const noexcept { return fds_[0]; }
            int write_fd() const noexcept { return fds_[1]; }

            void close_read() noexcept { close_fd(fds_[0]); }
            void close_write() noexcept { close_fd(fds_[1]); }

        private:
            static void close_fd(int& fd) noexcept {
                if (fd >= 0) {
                    static_cast<void>(::close(fd));
                    fd = -1;
                }
            }

            std::array<int, 2> fds_{{-1, -1}};
        };

        void append_available(int fd, std::string& destination, bool& open) {
            std::array<char, 4096> buffer{};
            while (open) {
                const ssize_t count = ::read(fd, buffer.data(), buffer.size());
                if (count > 0) {
                    if (destination.size()
                        > max_process_output - static_cast<std::size_t>(count)) {
                        throw BinanceAgenticWalletResponseError(
                            "baw response exceeded the output limit");
                    }
                    destination.append(
                        buffer.data(), static_cast<std::size_t>(count));
                    continue;
                }
                if (count == 0) {
                    open = false;
                    return;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                throw BinanceAgenticWalletTransportError(
                    std::string("failed to read baw output: ")
                    + std::strerror(errno));
            }
        }

        ProcessResult run_process(
            const BinanceAgenticWalletCliConfig& config,
            const std::vector<std::string>& arguments) {
            Pipe standard_output;
            Pipe standard_error;
            posix_spawn_file_actions_t actions;
            if (::posix_spawn_file_actions_init(&actions) != 0) {
                throw BinanceAgenticWalletTransportError(
                    "failed to initialize baw process actions");
            }

            const auto destroy_actions = [&actions] {
                static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
            };
            if (::posix_spawn_file_actions_adddup2(
                    &actions, standard_output.write_fd(), STDOUT_FILENO) != 0
                || ::posix_spawn_file_actions_adddup2(
                    &actions, standard_error.write_fd(), STDERR_FILENO) != 0
                || ::posix_spawn_file_actions_addclose(
                    &actions, standard_output.read_fd()) != 0
                || ::posix_spawn_file_actions_addclose(
                    &actions, standard_error.read_fd()) != 0) {
                destroy_actions();
                throw BinanceAgenticWalletTransportError(
                    "failed to configure baw process output");
            }

            std::vector<std::string> owned_arguments;
            owned_arguments.reserve(arguments.size() + 1);
            owned_arguments.push_back(config.baw_path);
            owned_arguments.insert(
                owned_arguments.end(), arguments.begin(), arguments.end());
            std::vector<char*> argv;
            argv.reserve(owned_arguments.size() + 1);
            for (std::string& argument : owned_arguments) {
                argv.push_back(argument.data());
            }
            argv.push_back(nullptr);

            pid_t child{};
            const int spawn_error = ::posix_spawnp(
                &child,
                config.baw_path.c_str(),
                &actions,
                nullptr,
                argv.data(),
                environ);
            destroy_actions();
            if (spawn_error != 0) {
                throw BinanceAgenticWalletTransportError(
                    std::string("failed to start baw: ")
                    + std::strerror(spawn_error));
            }
            standard_output.close_write();
            standard_error.close_write();

            ProcessResult result;
            bool stdout_open = true;
            bool stderr_open = true;
            bool child_exited = false;
            int child_status{};
            const auto deadline = std::chrono::steady_clock::now()
                + config.timeout;

            try {
                while (!child_exited || stdout_open || stderr_open) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        static_cast<void>(::kill(child, SIGKILL));
                        while (::waitpid(child, &child_status, 0) < 0
                               && errno == EINTR) {
                        }
                        throw BinanceAgenticWalletTransportError(
                            "baw command timed out");
                    }

                    std::array<pollfd, 2> descriptors{{
                        {standard_output.read_fd(),
                         static_cast<short>(stdout_open ? POLLIN : 0), 0},
                        {standard_error.read_fd(),
                         static_cast<short>(stderr_open ? POLLIN : 0), 0},
                    }};
                    int poll_result;
                    do {
                        poll_result = ::poll(
                            descriptors.data(), descriptors.size(), 25);
                    } while (poll_result < 0 && errno == EINTR);
                    if (poll_result < 0) {
                        throw BinanceAgenticWalletTransportError(
                            std::string("failed while waiting for baw: ")
                            + std::strerror(errno));
                    }

                    if (stdout_open
                        && (descriptors[0].revents
                            & (POLLIN | POLLHUP | POLLERR)) != 0) {
                        append_available(
                            standard_output.read_fd(),
                            result.standard_output,
                            stdout_open);
                    }
                    if (stderr_open
                        && (descriptors[1].revents
                            & (POLLIN | POLLHUP | POLLERR)) != 0) {
                        append_available(
                            standard_error.read_fd(),
                            result.standard_error,
                            stderr_open);
                    }

                    if (!child_exited) {
                        pid_t waited;
                        do {
                            waited = ::waitpid(child, &child_status, WNOHANG);
                        } while (waited < 0 && errno == EINTR);
                        if (waited < 0) {
                            throw BinanceAgenticWalletTransportError(
                                std::string("failed to wait for baw: ")
                                + std::strerror(errno));
                        }
                        child_exited = waited == child;
                    }
                }
            } catch (...) {
                if (!child_exited) {
                    static_cast<void>(::kill(child, SIGKILL));
                    while (::waitpid(child, &child_status, 0) < 0
                           && errno == EINTR) {
                    }
                }
                throw;
            }

            if (WIFEXITED(child_status)) {
                result.exit_code = WEXITSTATUS(child_status);
            } else {
                throw BinanceAgenticWalletTransportError(
                    "baw process ended abnormally");
            }
            return result;
        }

        Json parse_cli_json(const ProcessResult& process) {
            Json response;
            try {
                response = Json::parse(process.standard_output);
            } catch (const Json::exception&) {
                throw BinanceAgenticWalletResponseError(
                    "baw returned malformed JSON");
            }
            if (!response.is_object()) {
                throw BinanceAgenticWalletResponseError(
                    "baw response was not an object");
            }
            return response;
        }

        std::string error_name(const Json& response) {
            const auto error = response.find("error");
            if (error == response.end() || !error->is_object()) {
                return {};
            }
            const auto name = error->find("name");
            if (name == error->end() || !name->is_string()) {
                return {};
            }
            return name->get<std::string>();
        }

        [[noreturn]] void throw_cli_failure(
            const ProcessResult& process,
            const Json& response) {
            const std::string name = error_name(response);
            if (name == "NOT_LOGGED_IN" || name == "SESSION_NOT_FOUND") {
                throw BinanceAgenticWalletNoSessionError(
                    "Binance Agentic Wallet has no active session");
            }
            if (name == "SESSION_EXPIRED" || name == "UNAUTHORIZED") {
                throw BinanceAgenticWalletAuthenticationError(
                    "Binance Agentic Wallet session is expired or unauthorized");
            }
            if (name == "NETWORK_ERROR" || name == "REQUEST_TIMEOUT"
                || name == "SERVICE_UNAVAILABLE" || name == "DNS_RESOLVE_FAILED"
                || name == "SSL_ERROR") {
                throw BinanceAgenticWalletTransportError(
                    "Binance Agentic Wallet provider transport failed");
            }
            if (process.exit_code != 0 || response.value("success", false) == false) {
                throw BinanceAgenticWalletProviderError(
                    name.empty() ? "baw command failed"
                                 : "baw command failed: " + name);
            }
            throw BinanceAgenticWalletProviderError("baw command failed");
        }

        Json run_baw(
            const BinanceAgenticWalletCliConfig& config,
            const std::vector<std::string>& arguments) {
            ProcessResult process = run_process(config, arguments);
            Json response = parse_cli_json(process);
            const auto success = response.find("success");
            if (success == response.end() || !success->is_boolean()) {
                throw BinanceAgenticWalletResponseError(
                    "baw response is missing boolean success status");
            }
            if (process.exit_code != 0 || !success->get<bool>()) {
                throw_cli_failure(process, response);
            }
            return response;
        }

        const Json& require_object_member(
            const Json& object, std::string_view field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_object()) {
                throw BinanceAgenticWalletResponseError(
                    "baw response is missing object field " + std::string(field));
            }
            return *value;
        }

        std::string require_string_member(
            const Json& object, std::string_view field) {
            const auto value = object.find(field);
            if (value == object.end() || !value->is_string()
                || value->get_ref<const std::string&>().empty()) {
                throw BinanceAgenticWalletResponseError(
                    "baw response is missing string field " + std::string(field));
            }
            return value->get<std::string>();
        }

        void validate_requirement(const ExternalPaymentRequirement& requirement) {
            if (requirement.x402_version != 2 || requirement.resource_url.empty()
                || requirement.resource_description.empty()
                || requirement.resource_mime_type.empty()
                || requirement.scheme.empty() || requirement.network.empty()
                || requirement.amount.empty() || requirement.asset.empty()
                || requirement.pay_to.empty()
                || requirement.max_timeout_seconds == 0) {
                throw std::invalid_argument(
                    "wallet preview requires a complete x402 V2 requirement");
            }
        }

        std::string encode_requirement(
            const ExternalPaymentRequirement& requirement,
            const std::optional<BinanceX402AcceptExtra>& accept_extra) {
            Json accepted{
                {"scheme", requirement.scheme},
                {"network", requirement.network},
                {"amount", requirement.amount},
                {"asset", requirement.asset},
                {"payTo", requirement.pay_to},
                {"maxTimeoutSeconds", requirement.max_timeout_seconds},
            };
            if (accept_extra.has_value()) {
                accepted["extra"] = {
                    {"name", accept_extra->name},
                    {"version", accept_extra->version},
                };
            }
            return Json{
                {"x402Version", requirement.x402_version},
                {"resource",
                 {{"url", requirement.resource_url},
                  {"description", requirement.resource_description},
                  {"mimeType", requirement.resource_mime_type}}},
                {"accepts", Json::array({std::move(accepted)})},
            }.dump();
        }

        bool matches_requirement(
            const Json& option,
            const ExternalPaymentRequirement& requirement) {
            const auto original = option.find("originalAccept");
            if (original == option.end() || !original->is_object()) {
                return false;
            }
            return original->value("scheme", "") == requirement.scheme
                && original->value("network", "") == requirement.network
                && original->value("amount", "") == requirement.amount
                && original->value("asset", "") == requirement.asset
                && original->value("payTo", "") == requirement.pay_to;
        }

        void validate_optional_string_echo(
            const Json& option,
            std::string_view field,
            std::string_view expected) {
            const auto value = option.find(field);
            if (value == option.end() || value->is_null()) {
                return;
            }
            if (!value->is_string()) {
                throw BinanceAgenticWalletResponseError(
                    "baw preview has malformed optional field "
                    + std::string(field));
            }
            if (value->get_ref<const std::string&>() != expected) {
                throw BinanceAgenticWalletResponseError(
                    "baw preview contradicts payment requirement field "
                    + std::string(field));
            }
        }

        ExternalPaymentPreview normalize_preview(
            const Json& response,
            const ExternalPaymentRequirement& requirement) {
            const Json& data = require_object_member(response, "data");
            static_cast<void>(require_string_member(data, "paymentId"));
            const auto options = data.find("options");
            if (options == data.end() || !options->is_array()) {
                throw BinanceAgenticWalletResponseError(
                    "baw preview response is missing payment options");
            }
            const Json* selected = nullptr;
            for (const Json& option : *options) {
                if (option.is_object()
                    && matches_requirement(option, requirement)) {
                    selected = &option;
                    break;
                }
            }
            if (selected == nullptr) {
                throw BinanceAgenticWalletResponseError(
                    "baw preview originalAccept contradicts the payment requirement");
            }

            validate_optional_string_echo(
                *selected, "scheme", requirement.scheme);
            validate_optional_string_echo(
                *selected, "tokenAddress", requirement.asset);
            validate_optional_string_echo(
                *selected, "payTo", requirement.pay_to);

            const std::string provider_status =
                require_string_member(*selected, "status");
            ExternalPaymentPreviewStatus status;
            if (provider_status == "READY_TO_SIGN") {
                status = ExternalPaymentPreviewStatus::Approved;
            } else if (provider_status == "ACTION_REQUIRED"
                       || provider_status == "NOT_SIGNABLE") {
                status = ExternalPaymentPreviewStatus::Rejected;
            } else {
                throw BinanceAgenticWalletResponseError(
                    "baw preview returned an unknown option status");
            }

            std::vector<std::string> reasons;
            const auto reason_values = selected->find("reasons");
            if (reason_values == selected->end() || !reason_values->is_array()) {
                throw BinanceAgenticWalletResponseError(
                    "baw preview response has invalid reasons");
            }
            for (const Json& reason : *reason_values) {
                if (!reason.is_string()) {
                    throw BinanceAgenticWalletResponseError(
                        "baw preview response has a non-string reason");
                }
                reasons.push_back(reason.get<std::string>());
            }

            return ExternalPaymentPreview{
                status,
                "binance-agentic-wallet",
                requirement.network,
                requirement.asset,
                requirement.amount,
                provider_status,
                std::move(reasons),
            };
        }
    }  // namespace

    BinanceAgenticWalletCliBridge::BinanceAgenticWalletCliBridge(
        BinanceAgenticWalletCliConfig config)
        : config_(std::move(config)) {
        if (config_.baw_path.empty()) {
            throw std::invalid_argument("baw path must not be empty");
        }
        if (config_.timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("baw timeout must be positive");
        }
        if (config_.accept_extra.has_value()
            && (config_.accept_extra->name.empty()
                || config_.accept_extra->version.empty())) {
            throw std::invalid_argument(
                "Binance x402 accept extra fields must not be empty");
        }
    }

    ExternalPaymentPreview BinanceAgenticWalletCliBridge::preview(
        const ExternalPaymentRequirement& requirement) {
        validate_requirement(requirement);

        const Json status_response = run_baw(
            config_, {"wallet", "status", "--json"});
        const Json& status_data = require_object_member(status_response, "data");
        if (require_string_member(status_data, "status") != "CONNECTED") {
            throw BinanceAgenticWalletNoSessionError(
                "Binance Agentic Wallet is not connected");
        }

        const Json preview_response = run_baw(
            config_,
            {"x402-payment",
             "preview",
             "--paymentRequirements",
             encode_requirement(requirement, config_.accept_extra),
             "--json"});
        return normalize_preview(preview_response, requirement);
    }

    PreviewAuthorizedAccessGate::PreviewAuthorizedAccessGate(
        std::string protected_output)
        : protected_output_(std::move(protected_output)) {
        if (protected_output_.empty()) {
            throw std::invalid_argument("protected output must not be empty");
        }
    }

    bool PreviewAuthorizedAccessGate::apply_preview(
        const ExternalPaymentPreview& preview) noexcept {
        preview_authorized_ =
            preview.status == ExternalPaymentPreviewStatus::Approved;
        return preview_authorized_;
    }

    bool PreviewAuthorizedAccessGate::preview_authorized() const noexcept {
        return preview_authorized_;
    }

    std::optional<std::string> PreviewAuthorizedAccessGate::access() const {
        if (!preview_authorized_) {
            return std::nullopt;
        }
        return protected_output_;
    }
}  // namespace exchange
