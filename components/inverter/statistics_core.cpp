#include "sol.hpp"
namespace sol {
StatisticsAccumulator::StatisticsAccumulator(const Totals &t, time_t now) {
    today_ = dayKey(now);
    tm d{};
    localtime_r(&now, &d);
    d.tm_hour = d.tm_min = d.tm_sec = 0;
    --d.tm_mday;
    d.tm_isdst = -1;
    yesterday_ = dayKey(mktime(&d));
    localtime_r(&now, &d);
    d.tm_mday -= (d.tm_wday + 6) % 7;
    d.tm_isdst = -1;
    week_ = dayKey(mktime(&d));
    result_.totalWh = t.totalWh;
    if (t.day == today_) {
        result_.todayWh = result_.weekWh = result_.monthWh = result_.yearWh = t.dayWh;
        result_.peakToday = t.peak;
        result_.peakTime = t.peakTime;
        result_.bestDayWh = t.dayWh;
        result_.bestDay = t.day;
    }
}
void StatisticsAccumulator::add(const Record &r) {
    if (r.resolution != Resolution::Day || r.day >= today_ || !std::isfinite(r.energyWh) || r.energyWh < 0)
        return;
    if (r.day == yesterday_)
        result_.yesterdayWh = r.energyWh;
    if (r.day >= week_)
        result_.weekWh += r.energyWh;
    if (r.day / 100 == today_ / 100)
        result_.monthWh += r.energyWh;
    if (r.day / 10000 == today_ / 10000)
        result_.yearWh += r.energyWh;
    if (!std::isfinite(result_.bestDayWh) || r.energyWh > result_.bestDayWh) {
        result_.bestDayWh = r.energyWh;
        result_.bestDay = r.day;
    }
    sum_ += r.energyWh;
    ++result_.completedDays;
}
StatisticsSummary StatisticsAccumulator::result() const {
    auto out = result_;
    if (out.completedDays)
        out.averageWh = sum_ / out.completedDays;
    return out;
}
} // namespace sol
