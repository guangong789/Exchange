#include "agent/provider/deepseek/deepseek_client.hpp"

#include <climits>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace exchange {
    namespace {
        constexpr std::size_t max_response_size = 1024 * 1024;

        class CurlGlobalState {
        public:
            CurlGlobalState() {
                if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                    throw DeepSeekDecisionError(
                        "Failed to initialize DeepSeek HTTP transport");
                }
            }

            ~CurlGlobalState() {
                curl_global_cleanup();
            }
        };

        struct ResponseBuffer {
            std::string content;
            bool exceeded_limit{};
        };

        std::size_t append_response(
            char* data,
            std::size_t size,
            std::size_t count,
            void* context) noexcept {
            if (size != 0 && count > max_response_size / size) {
                static_cast<ResponseBuffer*>(context)->exceeded_limit = true;
                return 0;
            }
            const std::size_t byte_count = size * count;
            auto& response = *static_cast<ResponseBuffer*>(context);
            if (byte_count > max_response_size - response.content.size()) {
                response.exceeded_limit = true;
                return 0;
            }
            try {
                response.content.append(data, byte_count);
                return byte_count;
            } catch (...) {
                return 0;
            }
        }

        CurlGlobalState& curl_global_state() {
            static CurlGlobalState state;
            return state;
        }
    }  // namespace

    DeepSeekClientConfig deepseek_config_from_environment(
        DeepSeekClientConfig config) {
        const char* api_key = std::getenv("DEEPSEEK_API_KEY");
        if (api_key == nullptr || api_key[0] == '\0') {
            throw DeepSeekDecisionError(
                "DEEPSEEK_API_KEY is not set");
        }
        config.api_key = api_key;
        return config;
    }

    DeepSeekClient::DeepSeekClient(DeepSeekClientConfig config)
        : config_(std::move(config)) {
        if (config_.endpoint.empty()) {
            throw std::invalid_argument(
                "DeepSeek endpoint must be non-empty");
        }
        if (config_.model.empty()) {
            throw std::invalid_argument(
                "DeepSeek model must be non-empty");
        }
        if (config_.timeout.count() <= 0
            || config_.timeout.count() > LONG_MAX) {
            throw std::invalid_argument(
                "DeepSeek timeout must be positive and in range");
        }
        if (config_.api_key.empty()) {
            throw std::invalid_argument(
                "DeepSeek API key must be provided");
        }
        static_cast<void>(curl_global_state());
    }

    std::string DeepSeekClient::complete(
        const DeepSeekPrompt& prompt) const {
        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        using CurlHeaders =
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

        CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
        if (!handle) {
            throw DeepSeekDecisionError(
                "Failed to create DeepSeek HTTP request");
        }

        const nlohmann::json request_json = {
            {"model", config_.model},
            {"messages",
             {{{"role", "system"}, {"content", prompt.system}},
              {{"role", "user"}, {"content", prompt.user}}}},
            {"response_format", {{"type", "json_object"}}},
            {"thinking", {{"type", "disabled"}}},
            {"temperature", 0},
            {"max_tokens", 128},
            {"stream", false},
        };
        const std::string request_body = request_json.dump();
        const std::string authorization =
            "Authorization: Bearer " + config_.api_key;

        CurlHeaders headers(nullptr, &curl_slist_free_all);
        const auto append_header = [&headers](const char* value) {
            curl_slist* updated = curl_slist_append(headers.get(), value);
            if (updated == nullptr) {
                throw DeepSeekDecisionError(
                    "Failed to allocate DeepSeek HTTP headers");
            }
            static_cast<void>(headers.release());
            headers.reset(updated);
        };
        append_header("Content-Type: application/json");
        append_header(authorization.c_str());

        ResponseBuffer response;
        char error_buffer[CURL_ERROR_SIZE]{};
        const auto set_option = [&handle](CURLoption option, auto value) {
            if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) {
                throw DeepSeekDecisionError(
                    "Failed to configure DeepSeek HTTP request");
            }
        };
        set_option(CURLOPT_URL, config_.endpoint.c_str());
        set_option(CURLOPT_HTTPHEADER, headers.get());
        set_option(CURLOPT_POST, 1L);
        set_option(CURLOPT_POSTFIELDS, request_body.c_str());
        set_option(
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(request_body.size()));
        set_option(CURLOPT_WRITEFUNCTION, &append_response);
        set_option(CURLOPT_WRITEDATA, &response);
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
                throw DeepSeekDecisionError(
                    "DeepSeek response exceeded the size limit");
            }
            const char* detail = error_buffer[0] == '\0'
                ? curl_easy_strerror(result)
                : error_buffer;
            throw DeepSeekDecisionError(
                std::string("DeepSeek transport failure: ") + detail);
        }

        long status = 0;
        if (curl_easy_getinfo(
                handle.get(),
                CURLINFO_RESPONSE_CODE,
                &status)
            != CURLE_OK) {
            throw DeepSeekDecisionError(
                "Failed to read DeepSeek HTTP status");
        }
        if (status < 200 || status >= 300) {
            throw DeepSeekDecisionError(
                "DeepSeek API returned HTTP status "
                + std::to_string(status));
        }

        try {
            const nlohmann::json response_json =
                nlohmann::json::parse(response.content);
            const auto choices = response_json.find("choices");
            if (choices == response_json.end() || !choices->is_array()
                || choices->size() != 1) {
                throw DeepSeekDecisionError(
                    "DeepSeek response has invalid choices");
            }
            const auto message = (*choices)[0].find("message");
            if (message == (*choices)[0].end() || !message->is_object()) {
                throw DeepSeekDecisionError(
                    "DeepSeek response has no assistant message");
            }
            const auto content = message->find("content");
            if (content == message->end() || !content->is_string()
                || content->get_ref<const std::string&>().empty()) {
                throw DeepSeekDecisionError(
                    "DeepSeek response has no assistant content");
            }
            return content->get<std::string>();
        } catch (const DeepSeekDecisionError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw DeepSeekDecisionError(
                "DeepSeek API returned malformed JSON");
        }
    }
}  // namespace exchange
