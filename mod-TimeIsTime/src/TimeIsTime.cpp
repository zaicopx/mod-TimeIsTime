/*
 * Azerothcore Module:     TimeIsTime
 * Author:                 Dunjeon
 * Contributing Author(s): lasyan3, vratam @ RegWorks
 * Version:                20250608 (patched: float precision, config validation,
 *                         restart-proof continuous clock)
 * License:                GNU Affero General Public License v3.0.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "GameTime.h"
#include "Log.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace
{
    // Kecepatan jam game bawaan core (1/60) - sama dengan nilai di Player::SendInitialPacketsBeforeAddToMap
    constexpr float  GAME_SPEED_BASE = 0.01666667f;

    // Batas aman SpeedRate. 0 = waktu beku, negatif = waktu mundur.
    constexpr float  SPEED_RATE_MIN  = 0.01f;
    constexpr float  SPEED_RATE_MAX  = 1440.0f;   // 1 hari game = 1 menit real
    constexpr float  HOUR_OFFSET_MAX = 24.0f;

    constexpr int64  HOUR_SECONDS    = 3600;
    constexpr int64  DAY_SECONDS     = 86400;
}

static bool   stimeistime_enable,
              stimeistime_announce,
              stimeistime_continuous;
static float  stimeistime_speed_rate,
              stimeistime_hour_offset;
static uint32 stimeistime_time_start;

// Hitung waktu (epoch, detik) yang dikirim ke client lewat SMSG_LOGIN_SETTIMESPEED.
// Semua aritmetika memakai int64/double - float tidak cukup presisi untuk nilai epoch (~1,7 miliar).
//
//  now            : waktu real server (epoch, detik)
//  uptime         : detik sejak server start
//  localSecOfDay  : detik sejak tengah malam (waktu lokal server) pada saat 'now'
static int64 TimeIsTimeCalcClientTime(int64 now, int64 uptime, int64 localSecOfDay,
                                      double rate, double hourOffset, uint32 timeStart, bool continuous)
{
    int64 const offsetSec = static_cast<int64>(std::llround(hourOffset * HOUR_SECONDS));

    // Mode TimeStart: jam client dimulai dari epoch tetap setiap login (perilaku asli).
    if (timeStart > 0)
        return static_cast<int64>(timeStart) + offsetSec;

    // Mode lama: berbasis waktu start server. Jam game "lompat" setiap restart (perilaku asli).
    if (!continuous)
        return (now - uptime) + static_cast<int64>(std::llround(static_cast<double>(uptime) * rate));

    // Mode continuous (default): jam game murni fungsi dari waktu real, tidak tergantung uptime.
    // Hasilnya konsisten antar restart dan sama untuk semua pemain / bot.
    //
    // Offset zona waktu lokal server = jam lokal - jam UTC. Dipakai supaya rate 1.0 persis
    // sama dengan waktu lokal server (Blizzlike) seperti sebelumnya.
    int64 const utcSecOfDay = now % DAY_SECONDS;
    int64 const tz          = localSecOfDay - utcSecOfDay;

    double tod = std::fmod(static_cast<double>(now) * rate + static_cast<double>(tz + offsetSec),
                           static_cast<double>(DAY_SECONDS));
    if (tod < 0.0)
        tod += static_cast<double>(DAY_SECONDS);

    // Tanggal tetap tanggal real hari ini, hanya jam-nya yang dipercepat.
    // (Menghindari tanggal melompat jauh ke masa depan saat rate besar.)
    // Geser tanggal (+-1 hari) supaya hasil tetap dalam +-12 jam dari waktu real + offset.
    // Untuk rate 1.0 hasilnya persis waktu real (termasuk saat HourOffset melewati tengah malam).
    int64 const localMidnight = now - localSecOfDay;
    int64 result = localMidnight + static_cast<int64>(tod);
    int64 const ref = now + offsetSec;
    while (result - ref > DAY_SECONDS / 2)
        result -= DAY_SECONDS;
    while (ref - result > DAY_SECONDS / 2)
        result += DAY_SECONDS;
    return result;
}

class TimeIsTimeBeforeConfigLoad : public WorldScript {
public:

    TimeIsTimeBeforeConfigLoad() : WorldScript("TimeIsTimeBeforeConfigLoad") { }

    void OnBeforeConfigLoad(bool /*reload*/) override {
        stimeistime_enable     = sConfigMgr->GetOption<bool>("TimeIsTime.Enable", true);
        stimeistime_announce   = sConfigMgr->GetOption<bool>("TimeIsTime.Announce", true);
        stimeistime_continuous = sConfigMgr->GetOption<bool>("TimeIsTime.ContinuousClock", true);
        stimeistime_time_start = sConfigMgr->GetOption<uint32>("TimeIsTime.TimeStart", 0);

        float rate = sConfigMgr->GetOption<float>("TimeIsTime.SpeedRate", 1.0f);
        if (!std::isfinite(rate)) {
            LOG_WARN("module.timeistime", "TimeIsTime.SpeedRate tidak valid, memakai 1.0");
            rate = 1.0f;
        }
        else if (rate < SPEED_RATE_MIN || rate > SPEED_RATE_MAX) {
            float const clamped = std::clamp(rate, SPEED_RATE_MIN, SPEED_RATE_MAX);
            LOG_WARN("module.timeistime", "TimeIsTime.SpeedRate = {} di luar rentang {} - {}, dipakai {}",
                     rate, SPEED_RATE_MIN, SPEED_RATE_MAX, clamped);
            rate = clamped;
        }
        stimeistime_speed_rate = rate;

        float offset = sConfigMgr->GetOption<float>("TimeIsTime.HourOffset", 0.0f);
        if (!std::isfinite(offset)) {
            LOG_WARN("module.timeistime", "TimeIsTime.HourOffset tidak valid, memakai 0");
            offset = 0.0f;
        }
        else if (offset < -HOUR_OFFSET_MAX || offset > HOUR_OFFSET_MAX) {
            float const clamped = std::clamp(offset, -HOUR_OFFSET_MAX, HOUR_OFFSET_MAX);
            LOG_WARN("module.timeistime", "TimeIsTime.HourOffset = {} di luar rentang +-{}, dipakai {}",
                     offset, HOUR_OFFSET_MAX, clamped);
            offset = clamped;
        }
        stimeistime_hour_offset = offset;
    }
};

class TimeIsTime : public PlayerScript {
public:

    TimeIsTime() : PlayerScript("TimeIsTime") { }

    void OnPlayerLogin(Player* player) override {
        if (stimeistime_enable && stimeistime_announce)
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00TimeIsTime |rmodule");
    }

    void OnPlayerSendInitialPacketsBeforeAddToMap(Player* player, WorldPacket& data) override {
        if (!stimeistime_enable)
            return;

        int64 const now    = GameTime::GetGameTime().count();
        int64 const uptime = GameTime::GetUptime().count();

        std::tm const lt = Acore::Time::TimeBreakdown(static_cast<time_t>(now));
        int64 const localSecOfDay = static_cast<int64>(lt.tm_hour) * HOUR_SECONDS + lt.tm_min * 60 + lt.tm_sec;

        int64 const clientTime = TimeIsTimeCalcClientTime(now, uptime, localSecOfDay,
                                                          stimeistime_speed_rate, stimeistime_hour_offset,
                                                          stimeistime_time_start, stimeistime_continuous);

        data.Initialize(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
        data.AppendPackedTime(static_cast<time_t>(clientTime));
        data << float(GAME_SPEED_BASE * stimeistime_speed_rate);
        data << uint32(0);

        player->GetSession()->SendPacket(&data);
    }
};

void AddTimeIsTimeScripts() {
    new TimeIsTimeBeforeConfigLoad();
    new TimeIsTime();
}
