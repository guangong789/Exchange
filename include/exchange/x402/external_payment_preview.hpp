#pragma once

#include <string>
#include <vector>

namespace exchange {
    enum class ExternalPaymentPreviewStatus {
        Approved,
        Rejected,
    };

    struct ExternalPaymentPreview {
        ExternalPaymentPreviewStatus status{
            ExternalPaymentPreviewStatus::Rejected};
        std::string provider;
        std::string network;
        std::string asset;
        std::string amount;
        std::string provider_status;
        std::vector<std::string> reasons;

        bool operator==(const ExternalPaymentPreview&) const = default;
    };
}  // namespace exchange
