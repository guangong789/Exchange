#include "exchange/model/deepseek_adapter.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>
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
                    throw ModelAdapterError(
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
            const std::size_t byte_count = size * count;
            auto& response = *static_cast<ResponseBuffer*>(context);
            if (size != 0 && byte_count / size != count) {
                response.exceeded_limit = true;
                return 0;
            }
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

        std::string require_api_key() {
            const char* value = std::getenv("DEEPSEEK_API_KEY");
            if (value == nullptr || value[0] == '\0') {
                throw ModelAdapterError(
                    "DEEPSEEK_API_KEY is not set");
            }
            return value;
        }

        CurlGlobalState& curl_global_state() {
            static CurlGlobalState state;
            return state;
        }
    }  // namespace

    DeepSeekAdapter::DeepSeekAdapter(DeepSeekConfig config)
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
        api_key_ = require_api_key();
        static_cast<void>(curl_global_state());
    }

    ModelResponse DeepSeekAdapter::invoke(const ModelRequest& request) {
        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        using CurlHeaders =
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

        CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
        if (!handle) {
            throw ModelAdapterError(
                "Failed to create DeepSeek HTTP request");
        }

        const nlohmann::json request_json = {
            {"model", config_.model},
            {"messages",
             {{{"role", "system"}, {"content", request.system_prompt}},
              {{"role", "user"}, {"content", request.user_prompt}}}},
            {"response_format", {{"type", "json_object"}}},
            {"thinking", {{"type", "disabled"}}},
            {"temperature", 0},
            {"max_tokens", 128},
            {"stream", false},
        };
        const std::string request_body = request_json.dump();
        const std::string authorization = "Authorization: Bearer " + api_key_;

        CurlHeaders headers(nullptr, &curl_slist_free_all);
        const auto append_header = [&headers](const char* value) {
            curl_slist* updated = curl_slist_append(headers.get(), value);
            if (updated == nullptr) {
                throw ModelAdapterError(
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
                throw ModelAdapterError(
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
        set_option(CURLOPT_TIMEOUT_MS, static_cast<long>(config_.timeout.count()));
        set_option(
            CURLOPT_CONNECTTIMEOUT_MS,
            static_cast<long>(config_.timeout.count()));
        set_option(CURLOPT_NOSIGNAL, 1L);
        set_option(CURLOPT_ERRORBUFFER, error_buffer);

        const CURLcode result = curl_easy_perform(handle.get());
        if (result != CURLE_OK) {
            if (response.exceeded_limit) {
                throw ModelAdapterError(
                    "DeepSeek response exceeded the size limit");
            }
            const char* detail = error_buffer[0] == '\0'
                ? curl_easy_strerror(result)
                : error_buffer;
            throw ModelAdapterError(
                std::string("DeepSeek transport failure: ") + detail);
        }

        long status = 0;
        if (curl_easy_getinfo(
                handle.get(),
                CURLINFO_RESPONSE_CODE,
                &status)
            != CURLE_OK) {
            throw ModelAdapterError(
                "Failed to read DeepSeek HTTP status");
        }
        if (status < 200 || status >= 300) {
            throw ModelAdapterError(
                "DeepSeek API returned HTTP status "
                + std::to_string(status));
        }

        try {
            const nlohmann::json response_json =
                nlohmann::json::parse(response.content);
            const auto choices = response_json.find("choices");
            if (choices == response_json.end() || !choices->is_array()
                || choices->size() != 1) {
                throw ModelAdapterError(
                    "DeepSeek response has invalid choices");
            }
            const auto message = (*choices)[0].find("message");
            if (message == (*choices)[0].end() || !message->is_object()) {
                throw ModelAdapterError(
                    "DeepSeek response has no assistant message");
            }
            const auto content = message->find("content");
            if (content == message->end() || !content->is_string()
                || content->get_ref<const std::string&>().empty()) {
                throw ModelAdapterError(
                    "DeepSeek response has no assistant content");
            }
            return ModelResponse{content->get<std::string>()};
        } catch (const ModelAdapterError&) {
            throw;
        } catch (const nlohmann::json::exception&) {
            throw ModelAdapterError(
                "DeepSeek API returned malformed JSON");
        }
    }
}  // namespace exchange
