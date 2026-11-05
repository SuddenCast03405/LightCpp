#include "LightCpp/App/FindScan.hpp"

namespace LightCpp::App::FindScan {

int resumeAfter(int start, int length, int docEnd, bool backward)
{
    if (length > 0) {
        return start + length;
    }
    const int stepped = backward ? start - 1 : start + 1;
    return (stepped < 0 || stepped > docEnd) ? -1 : stepped;
}

}  // namespace LightCpp::App::FindScan
