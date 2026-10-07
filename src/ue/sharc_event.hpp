#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nr::ue::sharc
{

void ConfigureEventSink(
    const std::string &path,
    const std::string &runId,
    const std::string &sourceId,
    const std::string &mappingId,
    int64_t clockUncertaintyNs,
    std::size_t queueCapacity);

void ShutdownEventSink();

bool EventSinkEnabled();

void EmitRegistrationRequest(
    const std::string &ueRef,
    const std::string &trigger,
    int registrationCounter,
    const std::string &mmState);

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
    int64_t jitterNs);

void EmitRecoveryTrigger(
    const std::string &ueRef,
    const std::string &cause,
    int registrationCounter,
    const std::string &mmState);

} // namespace nr::ue::sharc
