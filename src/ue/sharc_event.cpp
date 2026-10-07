#include "sharc_event.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include <unistd.h>

#include <utils/json.hpp>

namespace nr::ue::sharc
{

namespace
{

std::string EscapeStrictJson(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);

    char escaped[7]{};

    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                std::snprintf(
                    escaped,
                    sizeof(escaped),
                    "\\u%04x",
                    static_cast<unsigned int>(c));
                out += escaped;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }

    return out;
}

void AppendStrictJson(const Json &json, std::ostream &out)
{
    switch (json.type())
    {
    case Json::Type::NULL_TYPE:
        out << "null";
        break;

    case Json::Type::STRING:
        out << '"' << EscapeStrictJson(json.str()) << '"';
        break;

    case Json::Type::BOOL:
    case Json::Type::NUMBER:
        out << json.str();
        break;

    case Json::Type::OBJECT: {
        out << '{';

        bool first = true;
        for (const auto &item : json)
        {
            if (!first)
                out << ',';

            first = false;

            out << '"'
                << EscapeStrictJson(item.first)
                << "\":";

            AppendStrictJson(item.second, out);
        }

        out << '}';
        break;
    }

    case Json::Type::ARRAY: {
        out << '[';

        bool first = true;
        for (const auto &item : json)
        {
            if (!first)
                out << ',';

            first = false;
            AppendStrictJson(item.second, out);
        }

        out << ']';
        break;
    }
    }
}

std::string DumpStrictJson(const Json &json)
{
    std::ostringstream out;
    AppendStrictJson(json, out);
    return out.str();
}

int64_t MonotonicNs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

int64_t RealtimeNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string ReadBootId()
{
    std::ifstream in{"/proc/sys/kernel/random/boot_id"};
    if (!in)
        throw std::runtime_error("SHARC event sink cannot read kernel boot_id");

    std::string value;
    std::getline(in, value);

    if (value.empty())
        throw std::runtime_error("SHARC event sink read empty kernel boot_id");

    return value;
}

class EventSink
{
  public:
    EventSink(
        std::string path,
        std::string runId,
        std::string sourceId,
        std::string mappingId,
        int64_t clockUncertaintyNs,
        std::size_t queueCapacity)
        : m_path{std::move(path)},
          m_statsPath{m_path + ".stats.json"},
          m_runId{std::move(runId)},
          m_sourceId{std::move(sourceId)},
          m_mappingId{std::move(mappingId)},
          m_bootId{ReadBootId()},
          m_pid{static_cast<int>(::getpid())},
          m_clockUncertaintyNs{clockUncertaintyNs},
          m_queueCapacity{queueCapacity}
    {
        if (m_path.empty())
            throw std::runtime_error("SHARC event log path is empty");
        if (m_runId.empty())
            throw std::runtime_error("SHARC run_id is empty");
        if (m_sourceId.empty())
            throw std::runtime_error("SHARC source_id is empty");
        if (m_mappingId.empty())
            throw std::runtime_error("SHARC clock mapping_id is empty");
        if (m_queueCapacity == 0)
            throw std::runtime_error("SHARC event queue capacity must be positive");

        m_out.open(m_path, std::ios::out | std::ios::trunc);
        if (!m_out)
            throw std::runtime_error("Cannot open SHARC event log: " + m_path);

        m_worker = std::thread(&EventSink::WorkerLoop, this);
    }

    ~EventSink()
    {
        {
            std::lock_guard<std::mutex> lock{m_mutex};
            m_stop = true;
        }

        m_cv.notify_one();

        if (m_worker.joinable())
            m_worker.join();

        m_out.flush();
        m_out.close();
    }

    Json NewBaseEvent(const std::string &eventType, const std::string &ueRef)
    {
        const uint64_t seq = m_sourceSeq.fetch_add(1) + 1;

        const int64_t monoNs = MonotonicNs();
        const int64_t realNs = RealtimeNs();

        return Json::Obj({
            {"schema_version", "1.0.0"},
            {"run_id", m_runId},
            {"source",
             Json::Obj({
                 {"component", "ueransim_ue"},
                 {"instance_id", m_sourceId},
                 {"boot_id", m_bootId},
                 {"pid", static_cast<int32_t>(m_pid)},
                 {"source_seq", static_cast<int64_t>(seq)},
             })},
            {"clock",
             Json::Obj({
                 {"monotonic_ns", std::to_string(monoNs)},
                 {"realtime_ns", std::to_string(realNs)},
                 {"mapping_id", m_mappingId},
                 {"uncertainty_ns",
                  m_clockUncertaintyNs >= 0
                      ? Json{std::to_string(m_clockUncertaintyNs)}
                      : Json{nullptr}},
             })},
            {"event_type", eventType},
            {"correlation",
             Json::Obj({
                 {"ue_ref", ueRef},
                 {"episode_id", nullptr},
                 {"attempt_id", nullptr},
                 {"context_epoch", nullptr},
                 {"local_cycle_id", nullptr},
             })},
        });
    }

    void EmitRegistrationRequest(
        const std::string &ueRef,
        const std::string &trigger,
        int registrationCounter,
        const std::string &mmState)
    {
        auto event = NewBaseEvent("UE_REG_REQUEST_TX", ueRef);

        event.put(
            "registration",
            Json::Obj({
                {"request_role", "unknown"},
                {"trigger", trigger},
                {"registration_counter", registrationCounter},
                {"mm_state", mmState},
            }));

        event.put("cause", "nas_to_rrc_enqueue_succeeded");

        Enqueue(DumpStrictJson(event));
    }

    void EmitTimerEvent(
        const std::string &ueRef,
        const std::string &timerName,
        const std::string &timerInstanceId,
        const std::string &phase,
        int64_t durationNs,
        const std::string &sourceClock,
        int64_t startSourceNs,
        int64_t deadlineSourceNs,
        uint64_t localIndex,
        int localCounter,
        int64_t jitterNs)
    {
        std::string eventType;
        if (phase == "start")
            eventType = "UE_TIMER_START";
        else if (phase == "cancel")
            eventType = "UE_TIMER_CANCEL";
        else if (phase == "expire")
            eventType = "UE_TIMER_EXPIRE";
        else
            throw std::runtime_error("Invalid SHARC timer phase: " + phase);

        auto event = NewBaseEvent(eventType, ueRef);

        event.put(
            "timer",
            Json::Obj({
                {"timer_name", timerName},
                {"timer_instance_id", timerInstanceId},
                {"phase", phase},
                {"duration_ns", std::to_string(durationNs)},
                {"source_clock", sourceClock},
                {"start_source_ns", std::to_string(startSourceNs)},
                {"deadline_source_ns", std::to_string(deadlineSourceNs)},
                {"policy_generation", static_cast<int32_t>(0)},
                {"local_index", static_cast<int64_t>(localIndex)},
                {"local_counter", static_cast<int32_t>(localCounter)},
                {"jitter_ns", std::to_string(jitterNs)},
            }));

        event.put("cause", nullptr);

        Enqueue(DumpStrictJson(event));
    }

    void EmitRecoveryTrigger(
        const std::string &ueRef,
        const std::string &cause,
        int registrationCounter,
        const std::string &mmState)
    {
        auto event = NewBaseEvent("UE_RECOVERY_TRIGGER", ueRef);

        event.put(
            "registration",
            Json::Obj({
                {"request_role", "native_recovery"},
                {"trigger", cause},
                {"registration_counter", registrationCounter},
                {"mm_state", mmState},
            }));

        event.put("cause", cause);

        Enqueue(DumpStrictJson(event));
    }

  private:
    void Enqueue(std::string line)
    {
        bool accepted = false;

        {
            std::lock_guard<std::mutex> lock{m_mutex};

            if (m_queue.size() < m_queueCapacity)
            {
                m_queue.push_back(std::move(line));
                accepted = true;
            }
        }

        if (accepted)
        {
            m_enqueued.fetch_add(1);
            m_cv.notify_one();
        }
        else
        {
            /*
             * source_seq was already allocated. The resulting sequence gap
             * is therefore independent evidence of producer-side loss.
             */
            m_dropped.fetch_add(1);
        }
    }

    void WorkerLoop()
    {
        auto nextStats =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);

        while (true)
        {
            std::vector<std::string> batch;
            bool done = false;

            {
                std::unique_lock<std::mutex> lock{m_mutex};

                m_cv.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [this] { return m_stop || !m_queue.empty(); });

                while (!m_queue.empty())
                {
                    batch.push_back(std::move(m_queue.front()));
                    m_queue.pop_front();
                }

                done = m_stop && m_queue.empty();
            }

            for (const auto &line : batch)
            {
                m_out << line << '\n';
                m_written.fetch_add(1);
            }

            if (!batch.empty())
            {
                m_out.flush();
                if (!m_out)
                    m_writerErrors.fetch_add(1);
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextStats)
            {
                WriteStats(false);
                nextStats = now + std::chrono::seconds(1);
            }

            if (done)
                break;
        }

        m_out.flush();
        if (!m_out)
            m_writerErrors.fetch_add(1);

        WriteStats(true);
    }

    void WriteStats(bool shutdownComplete)
    {
        std::size_t queueDepth = 0;

        {
            std::lock_guard<std::mutex> lock{m_mutex};
            queueDepth = m_queue.size();
        }

        Json stats = Json::Obj({
            {"schema_version", "1.0.0"},
            {"run_id", m_runId},
            {"source_id", m_sourceId},
            {"boot_id", m_bootId},
            {"pid", static_cast<int32_t>(m_pid)},
            {"queue_capacity", static_cast<int64_t>(m_queueCapacity)},
            {"queue_depth", static_cast<int64_t>(queueDepth)},
            {"source_seq_high",
             static_cast<int64_t>(m_sourceSeq.load())},
            {"events_attempted",
             static_cast<int64_t>(m_sourceSeq.load())},
            {"events_enqueued",
             static_cast<int64_t>(m_enqueued.load())},
            {"events_written",
             static_cast<int64_t>(m_written.load())},
            {"events_dropped",
             static_cast<int64_t>(m_dropped.load())},
            {"writer_errors",
             static_cast<int64_t>(m_writerErrors.load())},
            {"shutdown_complete", shutdownComplete},
            {"shutdown_flush_ok",
             shutdownComplete ? Json{m_out.good()} : Json{nullptr}},
        });

        const std::string tmp = m_statsPath + ".tmp";

        {
            std::ofstream out{tmp, std::ios::out | std::ios::trunc};
            if (!out)
            {
                m_writerErrors.fetch_add(1);
                return;
            }

            out << DumpStrictJson(stats) << '\n';

            if (!out)
            {
                m_writerErrors.fetch_add(1);
                return;
            }
        }

        if (std::rename(tmp.c_str(), m_statsPath.c_str()) != 0)
            m_writerErrors.fetch_add(1);
    }

  private:
    std::string m_path;
    std::string m_statsPath;
    std::string m_runId;
    std::string m_sourceId;
    std::string m_mappingId;
    std::string m_bootId;

    int m_pid{};
    int64_t m_clockUncertaintyNs{};
    std::size_t m_queueCapacity{};

    std::ofstream m_out;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    bool m_stop{false};

    std::thread m_worker;

    std::atomic<uint64_t> m_sourceSeq{0};
    std::atomic<uint64_t> m_enqueued{0};
    std::atomic<uint64_t> m_written{0};
    std::atomic<uint64_t> m_dropped{0};
    std::atomic<uint64_t> m_writerErrors{0};
};

std::unique_ptr<EventSink> g_eventSink;

} // namespace

void ConfigureEventSink(
    const std::string &path,
    const std::string &runId,
    const std::string &sourceId,
    const std::string &mappingId,
    int64_t clockUncertaintyNs,
    std::size_t queueCapacity)
{
    if (g_eventSink)
        throw std::runtime_error("SHARC event sink already configured");

    g_eventSink = std::make_unique<EventSink>(
        path,
        runId,
        sourceId,
        mappingId,
        clockUncertaintyNs,
        queueCapacity);
}

void ShutdownEventSink()
{
    g_eventSink.reset();
}

bool EventSinkEnabled()
{
    return static_cast<bool>(g_eventSink);
}

void EmitRegistrationRequest(
    const std::string &ueRef,
    const std::string &trigger,
    int registrationCounter,
    const std::string &mmState)
{
    if (g_eventSink)
        g_eventSink->EmitRegistrationRequest(
            ueRef, trigger, registrationCounter, mmState);
}

void EmitTimerEvent(
    const std::string &ueRef,
    const std::string &timerName,
    const std::string &timerInstanceId,
    const std::string &phase,
    int64_t durationNs,
    const std::string &sourceClock,
    int64_t startSourceNs,
    int64_t deadlineSourceNs,
    uint64_t localIndex,
    int localCounter,
    int64_t jitterNs)
{
    if (g_eventSink)
        g_eventSink->EmitTimerEvent(
            ueRef,
            timerName,
            timerInstanceId,
            phase,
            durationNs,
            sourceClock,
            startSourceNs,
            deadlineSourceNs,
            localIndex,
            localCounter,
            jitterNs);
}

void EmitRecoveryTrigger(
    const std::string &ueRef,
    const std::string &cause,
    int registrationCounter,
    const std::string &mmState)
{
    if (g_eventSink)
        g_eventSink->EmitRecoveryTrigger(
            ueRef, cause, registrationCounter, mmState);
}

} // namespace nr::ue::sharc
