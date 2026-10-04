//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "udp_task.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

#include <gnb/nts.hpp>
#include <utils/common.hpp>
#include <utils/constants.hpp>
#include <utils/libc_error.hpp>

static constexpr const int BUFFER_SIZE = 16384;

static constexpr const int LOOP_PERIOD = 1000;
static constexpr const int RECEIVE_TIMEOUT = 200;
static constexpr const int HEARTBEAT_THRESHOLD = 2000; // (LOOP_PERIOD + RECEIVE_TIMEOUT)'dan büyük olmalı

static constexpr const int MIN_ALLOWED_DBM = -120;

static constexpr const size_t MAX_UE_COUNT = 1024;

static int EstimateSimulatedDbm(const Vector3 &myPos, const Vector3 &uePos)
{
    double deltaX = static_cast<double>(myPos.x) - static_cast<double>(uePos.x);
    double deltaY = static_cast<double>(myPos.y) - static_cast<double>(uePos.y);
    double deltaZ = static_cast<double>(myPos.z) - static_cast<double>(uePos.z);

    double distSq = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
    int distance = static_cast<int>(std::sqrt(distSq));
    if (distance == 0)
        return -1; // 0 may be confusing for people
    return -distance;
}

namespace nr::gnb
{

RlsUdpTask::RlsUdpTask(TaskBase *base, uint64_t sti, Vector3 phyLocation)
    : m_server{}, m_ctlTask{}, m_sti{sti}, m_phyLocation{phyLocation}, m_lastLoop{}, m_stiToUe{}, m_ueMap{}, m_newIdCounter{}
{
    m_logger = base->logBase->makeUniqueLogger("rls-udp");

    try
    {
        m_server = new udp::UdpServer(base->config->linkIp, cons::RadioLinkPort);
    }
    catch (const LibError &e)
    {
        m_logger->err("RLS failure [%s]", e.what());
        quit();
        return;
    }
}

void RlsUdpTask::onStart()
{
}

void RlsUdpTask::onLoop()
{
    auto current = utils::CurrentTimeMillis();
    if (current - m_lastLoop > LOOP_PERIOD)
    {
        m_lastLoop = current;
        heartbeatCycle(current);
    }

    uint8_t buffer[BUFFER_SIZE];
    InetAddress peerAddress;

    int size = m_server->Receive(buffer, BUFFER_SIZE, RECEIVE_TIMEOUT, peerAddress);
    if (size > 0)
    {
        auto rlsMsg = rls::DecodeRlsMessage(OctetView{buffer, static_cast<size_t>(size)});
        if (rlsMsg == nullptr)
            m_logger->err("Unable to decode RLS message");
        else
            receiveRlsPdu(peerAddress, std::move(rlsMsg));
    }
}

void RlsUdpTask::onQuit()
{
    delete m_server;
}

void RlsUdpTask::receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg)
{
    if (msg->msgType == rls::EMessageType::HEARTBEAT)
    {
        int dbm = EstimateSimulatedDbm(m_phyLocation, ((const rls::RlsHeartBeat &)*msg).simPos);
        if (dbm < MIN_ALLOWED_DBM)
        {
            // if the simulated signal strength is such low, then ignore this message
            return;
        }

        if (m_stiToUe.count(msg->sti))
        {
            int ueId = m_stiToUe[msg->sti];
            {
                std::lock_guard<std::mutex> lock(m_ueMapMutex);
                auto it = m_ueMap.find(ueId);
                if (it != m_ueMap.end())
                {
                    it->second.address = addr;
                    it->second.lastSeen = utils::CurrentTimeMillis();
                }
            }
        }
        else
        {
            int ueId{};
            {
                std::lock_guard<std::mutex> lock(m_ueMapMutex);
                if (m_ueMap.size() >= MAX_UE_COUNT)
                {
                    m_logger->warn("Max UE count reached, rejecting new UE");
                    return;
                }

                ueId = ++m_newIdCounter;
                m_ueMap[ueId].sti = msg->sti;
                m_ueMap[ueId].address = addr;
                m_ueMap[ueId].lastSeen = utils::CurrentTimeMillis();
            }

            m_stiToUe[msg->sti] = ueId;

            auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::SIGNAL_DETECTED);
            w->ueId = ueId;
            m_ctlTask->push(std::move(w));
        }

        rls::RlsHeartBeatAck ack{m_sti};
        ack.dbm = dbm;

        sendRlsPdu(addr, ack);
        return;
    }

    if (!m_stiToUe.count(msg->sti))
    {
        // if no HB received yet, and the message is not HB, then ignore the message
        return;
    }

    auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::RECEIVE_RLS_MESSAGE);
    w->ueId = m_stiToUe[msg->sti];
    w->msg = std::move(msg);
    m_ctlTask->push(std::move(w));
}

void RlsUdpTask::sendRlsPdu(const InetAddress &addr, const rls::RlsMessage &msg)
{
    OctetString stream;
    rls::EncodeRlsMessage(msg, stream);

    m_server->Send(addr, stream.data(), static_cast<size_t>(stream.length()));
}

void RlsUdpTask::heartbeatCycle(int64_t time)
{
    std::set<int> lostUeId{};
    std::set<uint64_t> lostSti{};

    {
        std::lock_guard<std::mutex> lock(m_ueMapMutex);
        for (auto &item : m_ueMap)
        {
            if (time - item.second.lastSeen > HEARTBEAT_THRESHOLD)
            {
                lostUeId.insert(item.first);
                lostSti.insert(item.second.sti);
            }
        }

        for (int ueId : lostUeId)
            m_ueMap.erase(ueId);
    }

    for (uint64_t sti : lostSti)
        m_stiToUe.erase(sti);

    for (int ueId : lostUeId)
    {
        auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::SIGNAL_LOST);
        w->ueId = ueId;
        m_ctlTask->push(std::move(w));
    }
}

void RlsUdpTask::initialize(NtsTask *ctlTask)
{
    m_ctlTask = ctlTask;
}

void RlsUdpTask::send(int ueId, const rls::RlsMessage &msg)
{
    if (ueId == 0)
    {
        std::vector<InetAddress> addresses;
        {
            std::lock_guard<std::mutex> lock(m_ueMapMutex);
            addresses.reserve(m_ueMap.size());
            for (const auto &ue : m_ueMap)
                addresses.push_back(ue.second.address);
        }

        for (const auto &address : addresses)
            sendRlsPdu(address, msg);
        return;
    }

    InetAddress address;
    {
        std::lock_guard<std::mutex> lock(m_ueMapMutex);
        auto it = m_ueMap.find(ueId);
        if (it == m_ueMap.end())
        {
            // ignore the message
            return;
        }
        address = it->second.address;
    }

    sendRlsPdu(address, msg);
}

} // namespace nr::gnb
