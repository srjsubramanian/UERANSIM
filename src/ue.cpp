//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <unistd.h>

#include <lib/app/base_app.hpp>
#include <lib/app/cli_base.hpp>
#include <lib/app/cli_cmd.hpp>
#include <lib/app/proc_table.hpp>
#include <lib/app/ue_ctl.hpp>
#include <ue/ue.hpp>
#include <ue/sharc_event.hpp>
#include <utils/common.hpp>
#include <utils/concurrent_map.hpp>
#include <utils/constants.hpp>
#include <utils/options.hpp>
#include <utils/yaml_utils.hpp>
#include <yaml-cpp/yaml.h>

static app::CliServer *g_cliServer = nullptr;
static nr::ue::UeConfig *g_refConfig = nullptr;
static ConcurrentMap<std::string, nr::ue::UserEquipment *> g_ueMap{};
static app::CliResponseTask *g_cliRespTask = nullptr;

static struct Options
{
    std::string configFile{};
    bool noRoutingConfigs{};
    bool disableCmd{};
    std::string imsi{};
    int count{};
    int tempo{};
    std::string scheduleFile{};
    std::string activationLog{};
    int64_t scheduleBarrierUnixNs{};

    std::string sharcEventLog{};
    int64_t sharcClockUncertaintyNs{-1};
} g_options{};

struct NwUeControllerCmd : NtsMessage
{
    enum PR
    {
        PERFORM_SWITCH_OFF,
    } present;

    // PERFORM_SWITCH_OFF
    nr::ue::UserEquipment *ue{};

    explicit NwUeControllerCmd(PR present) : NtsMessage(NtsMessageType::UE_CTL_COMMAND), present(present)
    {
    }
};

class UeControllerTask : public NtsTask
{
  protected:
    void onStart() override
    {
    }

    void onLoop() override
    {
        auto msg = take();
        if (msg == nullptr)
            return;
        if (msg->msgType == NtsMessageType::UE_CTL_COMMAND)
        {
            auto &w = dynamic_cast<NwUeControllerCmd &>(*msg);
            switch (w.present)
            {
            case NwUeControllerCmd::PERFORM_SWITCH_OFF: {
                std::string key{};
                g_ueMap.invokeForeach([&key, &w](auto &item) {
                    if (item.second == w.ue)
                        key = item.first;
                });

                if (key.empty())
                    return;

                if (g_ueMap.removeAndGetSize(key) == 0)
                    exit(0);

                delete w.ue;
                break;
            }
            }
        }
    }

    void onQuit() override
    {
    }
};

static UeControllerTask *g_controllerTask;

static nr::ue::UeConfig *ReadConfigYaml()
{
    auto *result = new nr::ue::UeConfig();
    auto config = YAML::LoadFile(g_options.configFile);

    result->hplmn.mcc = yaml::GetInt32(config, "mcc", 1, 999);
    yaml::GetString(config, "mcc", 3, 3);
    result->hplmn.mnc = yaml::GetInt32(config, "mnc", 0, 999);
    result->hplmn.isLongMnc = yaml::GetString(config, "mnc", 2, 3).size() == 3;
    if (yaml::HasField(config, "routingIndicator"))
        result->routingIndicator = yaml::GetString(config, "routingIndicator", 1, 4);

    for (auto &gnbSearchItem : yaml::GetSequence(config, "gnbSearchList"))
        result->gnbSearchList.push_back(gnbSearchItem.as<std::string>());

    if (yaml::HasField(config, "default-nssai"))
    {
        for (auto &sNssai : yaml::GetSequence(config, "default-nssai"))
        {
            SingleSlice s{};
            s.sst = yaml::GetInt32(sNssai, "sst", 0, 0xFF);
            if (yaml::HasField(sNssai, "sd"))
                s.sd = octet3{yaml::GetInt32(sNssai, "sd", 0, 0xFFFFFF)};
            result->defaultConfiguredNssai.slices.push_back(s);
        }
    }

    if (yaml::HasField(config, "configured-nssai"))
    {
        for (auto &sNssai : yaml::GetSequence(config, "configured-nssai"))
        {
            SingleSlice s{};
            s.sst = yaml::GetInt32(sNssai, "sst", 0, 0xFF);
            if (yaml::HasField(sNssai, "sd"))
                s.sd = octet3{yaml::GetInt32(sNssai, "sd", 0, 0xFFFFFF)};
            result->configuredNssai.slices.push_back(s);
        }
    }

    result->key = OctetString::FromHex(yaml::GetString(config, "key", 32, 32));
    result->opC = OctetString::FromHex(yaml::GetString(config, "op", 32, 32));
    result->amf = OctetString::FromHex(yaml::GetString(config, "amf", 4, 4));

    result->configureRouting = !g_options.noRoutingConfigs;

    // If we have multiple UEs in the same process, then log names should be separated.
    result->prefixLogger = g_options.count > 1;

    if (yaml::HasField(config, "supi"))
        result->supi = Supi::Parse(yaml::GetString(config, "supi"));
    if (yaml::HasField(config, "protectionScheme"))
        result->protectionScheme = yaml::GetInt32(config, "protectionScheme", 0, 255);
    if (yaml::HasField(config, "homeNetworkPublicKeyId"))
        result->homeNetworkPublicKeyId = yaml::GetInt32(config, "homeNetworkPublicKeyId", 0, 255);
    if (yaml::HasField(config, "homeNetworkPublicKey"))        
        result->homeNetworkPublicKey = OctetString::FromHex(yaml::GetString(config, "homeNetworkPublicKey", 64, 64)); 
    if (yaml::HasField(config, "imei"))
        result->imei = yaml::GetString(config, "imei", 15, 15);
    if (yaml::HasField(config, "imeiSv"))
        result->imeiSv = yaml::GetString(config, "imeiSv", 16, 16);
    if (yaml::HasField(config, "tunName"))
        result->tunName = yaml::GetString(config, "tunName", 1, 12);
    if (yaml::HasField(config, "tunNetmask"))
        result->tunNetmask = yaml::GetString(config, "tunNetmask", 9, 15);
    if (yaml::HasField(config, "useNamespace"))
        result->useNamespace = yaml::GetBool(config, "useNamespace");
    if (yaml::HasField(config, "nsNamePrefix"))
        result->nsNamePrefix = yaml::GetString(config, "nsNamePrefix", 1, 64);

    yaml::AssertHasField(config, "integrity");
    yaml::AssertHasField(config, "ciphering");

    result->supportedAlgs.nia1 = yaml::GetBool(config["integrity"], "IA1");
    result->supportedAlgs.nia2 = yaml::GetBool(config["integrity"], "IA2");
    result->supportedAlgs.nia3 = yaml::GetBool(config["integrity"], "IA3");
    result->supportedAlgs.nea1 = yaml::GetBool(config["ciphering"], "EA1");
    result->supportedAlgs.nea2 = yaml::GetBool(config["ciphering"], "EA2");
    result->supportedAlgs.nea3 = yaml::GetBool(config["ciphering"], "EA3");

    std::string opType = yaml::GetString(config, "opType");
    if (opType == "OP")
        result->opType = nr::ue::OpType::OP;
    else if (opType == "OPC")
        result->opType = nr::ue::OpType::OPC;
    else
        throw std::runtime_error("Invalid OP type: " + opType);

    if (yaml::HasField(config, "sessions"))
    {
        for (auto &sess : yaml::GetSequence(config, "sessions"))
        {
            nr::ue::SessionConfig s{};

            if (yaml::HasField(sess, "apn"))
                s.apn = yaml::GetString(sess, "apn");
            if (yaml::HasField(sess, "slice"))
            {
                auto slice = sess["slice"];
                s.sNssai = SingleSlice{};
                s.sNssai->sst = yaml::GetInt32(slice, "sst", 0, 0xFF);
                if (yaml::HasField(slice, "sd"))
                    s.sNssai->sd = octet3{yaml::GetInt32(slice, "sd", 0, 0xFFFFFF)};
            }

            std::string type = yaml::GetString(sess, "type");
            if (type == "IPv4")
                s.type = nas::EPduSessionType::IPV4;
            else if (type == "IPv6")
                s.type = nas::EPduSessionType::IPV6;
            else if (type == "IPv4v6")
                s.type = nas::EPduSessionType::IPV4V6;
            else if (type == "Ethernet")
                s.type = nas::EPduSessionType::ETHERNET;
            else if (type == "Unstructured")
                s.type = nas::EPduSessionType::UNSTRUCTURED;
            else
                throw std::runtime_error("Invalid PDU session type: " + type);

            s.isEmergency = false;

            result->defaultSessions.push_back(s);
        }
    }

    yaml::AssertHasField(config, "integrityMaxRate");
    {
        auto uplink = yaml::GetString(config["integrityMaxRate"], "uplink");
        auto downlink = yaml::GetString(config["integrityMaxRate"], "downlink");
        if (uplink != "full" && uplink != "64kbps")
            throw std::runtime_error("Invalid integrity protection maximum uplink data rate: " + uplink);
        if (downlink != "full" && downlink != "64kbps")
            throw std::runtime_error("Invalid integrity protection maximum downlink data rate: " + downlink);
        result->integrityMaxRate.uplinkFull = uplink == "full";
        result->integrityMaxRate.downlinkFull = downlink == "full";
    }

    yaml::AssertHasField(config, "uacAic");
    {
        result->uacAic.mps = yaml::GetBool(config["uacAic"], "mps");
        result->uacAic.mcs = yaml::GetBool(config["uacAic"], "mcs");
    }

    yaml::AssertHasField(config, "uacAcc");
    {
        result->uacAcc.normalCls = yaml::GetInt32(config["uacAcc"], "normalClass", 0, 9);
        result->uacAcc.cls11 = yaml::GetBool(config["uacAcc"], "class11");
        result->uacAcc.cls12 = yaml::GetBool(config["uacAcc"], "class12");
        result->uacAcc.cls13 = yaml::GetBool(config["uacAcc"], "class13");
        result->uacAcc.cls14 = yaml::GetBool(config["uacAcc"], "class14");
        result->uacAcc.cls15 = yaml::GetBool(config["uacAcc"], "class15");
    }

    return result;
}

static void ReadOptions(int argc, char **argv)
{
    opt::OptionsDescription desc{
        cons::Project, cons::Tag, "5G-SA UE implementation", cons::Owner, "nr-ue", {"-c <config-file> [option...]"}, {},
        true,          false};

    opt::OptionItem itemConfigFile = {'c', "config", "Use specified configuration file for UE", "config-file"};
    opt::OptionItem itemImsi = {'i', "imsi", "Use specified IMSI number instead of provided one", "imsi"};
    opt::OptionItem itemCount = {'n', "num-of-UE", "Generate specified number of UEs starting from the given IMSI",
                                 "num"};
    opt::OptionItem itemTempo = {'t', "tempo", "Starting delay in milliseconds for each of the UEs", "tempo"};
    opt::OptionItem itemSchedule = {'s', "schedule-file",
                                    "Start UEs from an absolute-offset CSV schedule", "schedule-file"};
    opt::OptionItem itemActivationLog = {'a', "activation-log",
                                         "Write measured UE activation timestamps to CSV", "activation-log"};
    opt::OptionItem itemScheduleBarrier = {'b', "schedule-barrier-unix-ns",
                                           "Common UTC barrier in Unix nanoseconds for schedule offsets",
                                           "unix-ns"};
    opt::OptionItem itemSharcEventLog = {'E', "sharc-event-log",
                                         "Write SHARC structured UE source events as JSONL",
                                         "path"};
    opt::OptionItem itemSharcClockUncertainty = {'U', "sharc-clock-uncertainty-ns",
                                                 "Clock mapping uncertainty bound for SHARC events",
                                                 "nanoseconds"};
    opt::OptionItem itemDisableCmd = {'l', "disable-cmd", "Disable command line functionality for this instance",
                                      std::nullopt};
    opt::OptionItem itemDisableRouting = {'r', "no-routing-config",
                                          "Do not auto configure routing for UE TUN interface", std::nullopt};

    desc.items.push_back(itemConfigFile);
    desc.items.push_back(itemImsi);
    desc.items.push_back(itemCount);
    desc.items.push_back(itemTempo);
    desc.items.push_back(itemSchedule);
    desc.items.push_back(itemActivationLog);
    desc.items.push_back(itemScheduleBarrier);
    desc.items.push_back(itemSharcEventLog);
    desc.items.push_back(itemSharcClockUncertainty);
    desc.items.push_back(itemDisableCmd);
    desc.items.push_back(itemDisableRouting);

    opt::OptionsResult opt{argc, argv, desc, false, nullptr};

    g_options.configFile = opt.getOption(itemConfigFile);
    g_options.noRoutingConfigs = opt.hasFlag(itemDisableRouting);
    if (opt.hasFlag(itemCount))
    {
        g_options.count = utils::ParseInt(opt.getOption(itemCount));
        if (g_options.count <= 0)
            throw std::runtime_error("Invalid number of UEs");
        if (g_options.count > 512)
            throw std::runtime_error("Number of UEs is too big");
    }
    else
    {
        g_options.count = 1;
    }

    if (opt.hasFlag(itemTempo))
        g_options.tempo = utils::ParseInt(opt.getOption(itemTempo));
    else
        g_options.tempo = 0;

    if (opt.hasFlag(itemSchedule))
        g_options.scheduleFile = opt.getOption(itemSchedule);

    if (opt.hasFlag(itemActivationLog))
        g_options.activationLog = opt.getOption(itemActivationLog);

    if (opt.hasFlag(itemScheduleBarrier))
    {
        std::string value = opt.getOption(itemScheduleBarrier);
        size_t parsed = 0;
        try
        {
            g_options.scheduleBarrierUnixNs = std::stoll(value, &parsed);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("Invalid schedule barrier Unix nanoseconds");
        }
        if (parsed != value.size() || g_options.scheduleBarrierUnixNs <= 0)
            throw std::runtime_error("Invalid schedule barrier Unix nanoseconds");
    }

    if (!g_options.scheduleFile.empty() && g_options.tempo != 0)
        throw std::runtime_error("--schedule-file and --tempo are mutually exclusive");

    if (g_options.scheduleFile.empty() &&
        (!g_options.activationLog.empty() || g_options.scheduleBarrierUnixNs != 0))
        throw std::runtime_error("--activation-log/--schedule-barrier-unix-ns require --schedule-file");

    if (!g_options.scheduleFile.empty() && g_options.activationLog.empty())
        g_options.activationLog = g_options.scheduleFile + ".actual.csv";

    if (opt.hasFlag(itemSharcEventLog))
        g_options.sharcEventLog = opt.getOption(itemSharcEventLog);

    if (opt.hasFlag(itemSharcClockUncertainty))
    {
        std::string value = opt.getOption(itemSharcClockUncertainty);
        size_t parsed = 0;

        try
        {
            g_options.sharcClockUncertaintyNs = std::stoll(value, &parsed);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error(
                "Invalid SHARC clock uncertainty nanoseconds");
        }

        if (parsed != value.size() ||
            g_options.sharcClockUncertaintyNs < 0)
        {
            throw std::runtime_error(
                "Invalid SHARC clock uncertainty nanoseconds");
        }
    }

    if (!g_options.sharcEventLog.empty() &&
        g_options.scheduleFile.empty())
    {
        throw std::runtime_error(
            "--sharc-event-log requires --schedule-file");
    }

    if (g_options.sharcEventLog.empty() &&
        g_options.sharcClockUncertaintyNs >= 0)
    {
        throw std::runtime_error(
            "--sharc-clock-uncertainty-ns requires --sharc-event-log");
    }

    g_options.imsi = {};
    if (opt.hasFlag(itemImsi))
    {
        g_options.imsi = opt.getOption(itemImsi);
        if (g_options.imsi.length() > 5 && g_options.imsi[0] == 'i' && g_options.imsi[1] == 'm' &&
            g_options.imsi[2] == 's' && g_options.imsi[3] == 'i' && g_options.imsi[4] == '-')
        {
            g_options.imsi = g_options.imsi.substr(5);
        }

        Supi::Parse("imsi-" + g_options.imsi); // validate the string by parsing
    }

    g_options.disableCmd = opt.hasFlag(itemDisableCmd);
}

static std::string LargeSum(std::string a, std::string b)
{
    if (a.length() > b.length())
        std::swap(a, b);

    std::string str;
    size_t n1 = a.length(), n2 = b.length();

    reverse(a.begin(), a.end());
    reverse(b.begin(), b.end());

    int carry = 0;
    for (size_t i = 0; i < n1; i++)
    {
        int sum = ((a[i] - '0') + (b[i] - '0') + carry);
        str.push_back(static_cast<char>((sum % 10) + '0'));
        carry = sum / 10;
    }
    for (size_t i = n1; i < n2; i++)
    {
        int sum = ((b[i] - '0') + carry);
        str.push_back(static_cast<char>((sum % 10) + '0'));
        carry = sum / 10;
    }
    if (carry)
        throw std::runtime_error("UE serial number overflow");
    reverse(str.begin(), str.end());
    return str;
}

static void IncrementNumber(std::string &s, int delta)
{
    s = LargeSum(s, std::to_string(delta));
}

struct ScheduleEntry
{
    int ueIndex{};
    int64_t offsetNs{};
};

struct SchedulePlan
{
    std::string runId{};
    std::string shardId{};
    std::vector<ScheduleEntry> entries{};
};

struct ActivationRecord
{
    uint64_t sourceSeq{};
    int ueIndex{};
    int64_t scheduledOffsetNs{};
    int64_t targetMonoNs{};
    int64_t constructBeginMonoNs{};
    int64_t constructBeginUtcNs{};
    int64_t startCallMonoNs{};
    int64_t startReturnMonoNs{};
    int64_t latenessNs{};
};

static std::vector<std::string> SplitCsv(const std::string &line)
{
    std::vector<std::string> result{};
    std::stringstream ss{line};
    std::string field{};
    while (std::getline(ss, field, ','))
        result.push_back(field);
    return result;
}

static int64_t ParseInt64Strict(const std::string &value, const std::string &field)
{
    size_t parsed = 0;
    int64_t result = 0;
    try
    {
        result = std::stoll(value, &parsed);
    }
    catch (const std::exception &)
    {
        throw std::runtime_error("Invalid " + field + " in schedule: " + value);
    }

    if (parsed != value.size())
        throw std::runtime_error("Invalid " + field + " in schedule: " + value);
    return result;
}

static SchedulePlan ReadSchedule(const std::string &path, int expectedCount)
{
    std::ifstream in{path};
    if (!in)
        throw std::runtime_error("Cannot open schedule file: " + path);

    std::string line{};
    if (!std::getline(in, line))
        throw std::runtime_error("Schedule file is empty: " + path);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    const std::string expectedHeader = "schema_version,run_id,shard_id,ue_index,offset_ns";
    if (line != expectedHeader)
        throw std::runtime_error("Unexpected schedule header: " + line);

    SchedulePlan plan{};
    std::vector<bool> seen(static_cast<size_t>(expectedCount), false);
    int64_t previousOffset = -1;
    int lineNo = 1;

    while (std::getline(in, line))
    {
        lineNo++;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        auto fields = SplitCsv(line);
        if (fields.size() != 5)
            throw std::runtime_error("Schedule line " + std::to_string(lineNo) + " must have 5 fields");
        if (fields[0] != "1")
            throw std::runtime_error("Unsupported schedule schema_version at line " + std::to_string(lineNo));
        if (fields[1].empty() || fields[2].empty())
            throw std::runtime_error("run_id and shard_id must be non-empty");

        if (plan.entries.empty())
        {
            plan.runId = fields[1];
            plan.shardId = fields[2];
        }
        else if (fields[1] != plan.runId || fields[2] != plan.shardId)
        {
            throw std::runtime_error("Mixed run_id/shard_id in schedule");
        }

        int64_t ueIndex64 = ParseInt64Strict(fields[3], "ue_index");
        int64_t offsetNs = ParseInt64Strict(fields[4], "offset_ns");

        if (ueIndex64 < 0 || ueIndex64 >= expectedCount)
            throw std::runtime_error("ue_index out of range at line " + std::to_string(lineNo));
        if (offsetNs < 0)
            throw std::runtime_error("offset_ns must be non-negative at line " + std::to_string(lineNo));
        if (offsetNs < previousOffset)
            throw std::runtime_error("schedule offsets must be nondecreasing");
        previousOffset = offsetNs;

        int ueIndex = static_cast<int>(ueIndex64);
        if (seen[static_cast<size_t>(ueIndex)])
            throw std::runtime_error("duplicate ue_index in schedule: " + std::to_string(ueIndex));
        seen[static_cast<size_t>(ueIndex)] = true;

        plan.entries.push_back({ueIndex, offsetNs});
    }

    if (static_cast<int>(plan.entries.size()) != expectedCount)
        throw std::runtime_error("schedule entry count does not match --num-of-UE");

    for (int i = 0; i < expectedCount; i++)
        if (!seen[static_cast<size_t>(i)])
            throw std::runtime_error("schedule missing ue_index: " + std::to_string(i));

    return plan;
}

static int64_t RealtimeNs()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

static int64_t MonotonicNs()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

static void SleepUntilMonotonicNs(int64_t targetNs)
{
    auto target = std::chrono::steady_clock::time_point{std::chrono::nanoseconds{targetNs}};
    std::this_thread::sleep_until(target);
}

static void WriteActivationLog(const SchedulePlan &plan, int64_t barrierUtcNs, int64_t armUtcNs,
                               int64_t armMonoNs, const std::vector<ActivationRecord> &records)
{
    std::ofstream out{g_options.activationLog, std::ios::out | std::ios::trunc};
    if (!out)
        throw std::runtime_error("Cannot open activation log: " + g_options.activationLog);

    out << "schema_version,run_id,shard_id,source_seq,ue_index,scheduled_offset_ns,"
           "barrier_utc_ns,arm_utc_ns,arm_mono_ns,target_mono_ns,"
           "construct_begin_mono_ns,construct_begin_utc_ns,start_call_mono_ns,"
           "start_return_mono_ns,lateness_ns\n";

    for (const auto &r : records)
    {
        out << "1," << plan.runId << "," << plan.shardId << "," << r.sourceSeq << "," << r.ueIndex << ","
            << r.scheduledOffsetNs << "," << barrierUtcNs << "," << armUtcNs << "," << armMonoNs << ","
            << r.targetMonoNs << "," << r.constructBeginMonoNs << "," << r.constructBeginUtcNs << ","
            << r.startCallMonoNs << "," << r.startReturnMonoNs << "," << r.latenessNs << "\n";
    }

    if (!out)
        throw std::runtime_error("Failed while writing activation log: " + g_options.activationLog);
}

static nr::ue::UeConfig *GetConfigByUe(int ueIndex)
{
    auto *c = new nr::ue::UeConfig();
    c->key = g_refConfig->key.copy();
    c->opC = g_refConfig->opC.copy();
    c->opType = g_refConfig->opType;
    c->amf = g_refConfig->amf.copy();
    c->imei = g_refConfig->imei;
    c->imeiSv = g_refConfig->imeiSv;
    c->supi = g_refConfig->supi;
    c->protectionScheme = g_refConfig->protectionScheme;
    c->homeNetworkPublicKey = g_refConfig->homeNetworkPublicKey.copy();
    c->homeNetworkPublicKeyId = g_refConfig->homeNetworkPublicKeyId;
    c->routingIndicator = g_refConfig->routingIndicator;
    c->tunName = g_refConfig->tunName;
    c->tunNetmask = g_refConfig->tunNetmask;
    c->useNamespace = g_refConfig->useNamespace;
    c->nsNamePrefix = g_refConfig->nsNamePrefix;
    c->hplmn = g_refConfig->hplmn;
    c->configuredNssai = g_refConfig->configuredNssai;
    c->defaultConfiguredNssai = g_refConfig->defaultConfiguredNssai;
    c->supportedAlgs = g_refConfig->supportedAlgs;
    c->gnbSearchList = g_refConfig->gnbSearchList;
    c->defaultSessions = g_refConfig->defaultSessions;
    c->configureRouting = g_refConfig->configureRouting;
    c->prefixLogger = g_refConfig->prefixLogger;
    c->integrityMaxRate = g_refConfig->integrityMaxRate;
    c->uacAic = g_refConfig->uacAic;
    c->uacAcc = g_refConfig->uacAcc;

    if (c->supi.has_value())
        IncrementNumber(c->supi->value, ueIndex);
    if (c->imei.has_value())
        IncrementNumber(*c->imei, ueIndex);
    if (c->imeiSv.has_value())
        IncrementNumber(*c->imeiSv, ueIndex);

    return c;
}

static void ReceiveCommand(app::CliMessage &msg)
{
    if (msg.value.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Result(msg.clientAddr, ""));
        return;
    }

    std::vector<std::string> tokens{};

    auto exp = opt::PerformExpansion(msg.value, tokens);
    if (exp != opt::ExpansionResult::SUCCESS)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Invalid command: " + msg.value));
        return;
    }

    if (tokens.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Empty command"));
        return;
    }

    std::string error{}, output{};
    auto cmd = app::ParseUeCliCommand(std::move(tokens), error, output);
    if (!error.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, error));
        return;
    }
    if (!output.empty())
    {
        g_cliServer->sendMessage(app::CliMessage::Result(msg.clientAddr, output));
        return;
    }
    if (cmd == nullptr)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, ""));
        return;
    }

    auto *ue = g_ueMap.getOrDefault(msg.nodeName);
    if (ue == nullptr)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Node not found: " + msg.nodeName));
        return;
    }

    ue->pushCommand(std::move(cmd), msg.clientAddr);
}

static void Loop()
{
    if (!g_cliServer)
    {
        ::pause();
        return;
    }

    auto msg = g_cliServer->receiveMessage();
    if (msg.type == app::CliMessage::Type::ECHO)
    {
        g_cliServer->sendMessage(msg);
        return;
    }

    if (msg.type != app::CliMessage::Type::COMMAND)
        return;

    if (msg.value.size() > 0xFFFF)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Command is too large"));
        return;
    }

    if (msg.nodeName.size() > 0xFFFF)
    {
        g_cliServer->sendMessage(app::CliMessage::Error(msg.clientAddr, "Node name is too large"));
        return;
    }

    ReceiveCommand(msg);
}

static class UeController : public app::IUeController
{
  public:
    void performSwitchOff(nr::ue::UserEquipment *ue) override
    {
        auto w = std::make_unique<NwUeControllerCmd>(NwUeControllerCmd::PERFORM_SWITCH_OFF);
        w->ue = ue;
        g_controllerTask->push(std::move(w));
    }
} g_ueController;

int main(int argc, char **argv)
{
    app::Initialize();

    try
    {
        ReadOptions(argc, argv);
        g_refConfig = ReadConfigYaml();
        if (g_options.imsi.length() > 0)
            g_refConfig->supi = Supi::Parse("imsi-" + g_options.imsi);
    }
    catch (const std::runtime_error &e)
    {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    std::cout << cons::Name << std::endl;

    g_controllerTask = new UeControllerTask();
    g_controllerTask->start();

    if (!g_options.disableCmd)
    {
        g_cliServer = new app::CliServer{};
        g_cliRespTask = new app::CliResponseTask(g_cliServer);
    }

    if (!g_options.scheduleFile.empty())
    {
        auto plan = ReadSchedule(g_options.scheduleFile, g_options.count);

        if (!g_options.sharcEventLog.empty())
        {
            const std::string sourceId = "sharc-ran-" + plan.shardId;
            const std::string mappingId =
                plan.runId + ":" + sourceId;

            nr::ue::sharc::ConfigureEventSink(
                g_options.sharcEventLog,
                plan.runId,
                sourceId,
                mappingId,
                g_options.sharcClockUncertaintyNs,
                65536);

            std::atexit(nr::ue::sharc::ShutdownEventSink);
        }

        std::vector<std::string> scheduledNodeNames{};
        scheduledNodeNames.reserve(static_cast<size_t>(g_options.count));
        for (int i = 0; i < g_options.count; i++)
        {
            auto *config = GetConfigByUe(i);
            scheduledNodeNames.push_back(config->getNodeName());
            delete config;
        }

        if (!g_options.disableCmd)
        {
            app::CreateProcTable(scheduledNodeNames, g_cliServer->assignedAddress().getPort());
            g_cliRespTask->start();
        }

        int64_t armMonoNs = MonotonicNs();
        int64_t armUtcNs = RealtimeNs();
        int64_t barrierUtcNs = g_options.scheduleBarrierUnixNs;
        if (barrierUtcNs == 0)
            barrierUtcNs = armUtcNs + 1000000000LL;
        if (barrierUtcNs <= armUtcNs)
            throw std::runtime_error("Schedule barrier is not in the future");

        int64_t barrierMonoNs = armMonoNs + (barrierUtcNs - armUtcNs);

        std::vector<ActivationRecord> records{};
        records.reserve(plan.entries.size());

        try
        {
            uint64_t sourceSeq = 0;
            for (const auto &entry : plan.entries)
            {
                int64_t targetMonoNs = barrierMonoNs + entry.offsetNs;
                SleepUntilMonotonicNs(targetMonoNs);

                ActivationRecord record{};
                record.sourceSeq = ++sourceSeq;
                record.ueIndex = entry.ueIndex;
                record.scheduledOffsetNs = entry.offsetNs;
                record.targetMonoNs = targetMonoNs;
                record.constructBeginMonoNs = MonotonicNs();
                record.constructBeginUtcNs = RealtimeNs();
                record.latenessNs = record.constructBeginMonoNs - targetMonoNs;

                auto *config = GetConfigByUe(entry.ueIndex);

                config->sharcUeRef =
                    "sharc-ran-" + plan.shardId +
                    ":ue-" + std::to_string(entry.ueIndex);

                /*
                 * Episode identifiers are run-scoped; run_id is carried
                 * separately by every structured event.
                 */
                config->sharcEpisodeId =
                    config->sharcUeRef + ":episode-0";

                auto *ue = new nr::ue::UserEquipment(config, &g_ueController, nullptr, g_cliRespTask);
                g_ueMap.put(config->getNodeName(), ue);

                record.startCallMonoNs = MonotonicNs();
                ue->start();
                record.startReturnMonoNs = MonotonicNs();
                records.push_back(record);
            }
        }
        catch (...)
        {
            WriteActivationLog(plan, barrierUtcNs, armUtcNs, armMonoNs, records);
            throw;
        }

        WriteActivationLog(plan, barrierUtcNs, armUtcNs, armMonoNs, records);
    }
    else
    {
        for (int i = 0; i < g_options.count; i++)
        {
            auto *config = GetConfigByUe(i);
            auto *ue = new nr::ue::UserEquipment(config, &g_ueController, nullptr, g_cliRespTask);
            g_ueMap.put(config->getNodeName(), ue);
        }

        if (!g_options.disableCmd)
        {
            app::CreateProcTable(g_ueMap, g_cliServer->assignedAddress().getPort());
            g_cliRespTask->start();
        }

        if (g_options.tempo != 0)
        {
            g_ueMap.invokeForeach([](const auto &ue) {
                ue.second->start();
                std::this_thread::sleep_for(std::chrono::milliseconds(g_options.tempo));
            });
        }
        else
        {
            g_ueMap.invokeForeach([](const auto &ue) { ue.second->start(); });
        }
    }

    while (true)
        Loop();
}
