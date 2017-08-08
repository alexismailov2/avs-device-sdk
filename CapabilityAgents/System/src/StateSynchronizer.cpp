/*
 * StateSynchronizer.cpp
 *
 * Copyright 2017 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "System/StateSynchronizer.h"
#include "System/NotifyingMessageRequest.h"

#include <AVSCommon/AVS/EventBuilder.h>
#include <AVSCommon/Utils/Logger/Logger.h>

namespace alexaClientSDK {
namespace capabilityAgents {
namespace system {

using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::avs;

/// String to identify log entries originating from this file.
static const std::string TAG("StateSynchronizer");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// String to identify the AVS namespace of the event we send.
static const std::string STATE_SYNCHRONIZER_NAMESPACE = "System";

/// String to identify the AVS name of the event we send.
static const std::string STATE_SYNCHRONIZER_NAME = "SynchronizeState";

std::shared_ptr<StateSynchronizer> StateSynchronizer::create(
        std::shared_ptr<ContextManagerInterface> contextManager,
        std::shared_ptr<MessageSenderInterface> messageSender) {
    if (!contextManager) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullContextManager"));
        return nullptr;
    }
    if (!messageSender) {
        ACSDK_ERROR(LX("createFailed").d("reason", "nullMessageSender"));
        return nullptr;
    }
    return std::shared_ptr<StateSynchronizer>(new StateSynchronizer(contextManager, messageSender));
}

void StateSynchronizer::addObserver(std::shared_ptr<StateSynchronizer::ObserverInterface> observer) {
    if (!observer) {
        ACSDK_ERROR(LX("addObserverFailed").d("reason", "nullObserver"));
        return;
    }
    std::lock_guard<std::mutex> observerLock(m_observerMutex);
    if (m_observers.insert(observer).second) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        observer->onStateChanged(m_state);
    } else {
        ACSDK_DEBUG(LX("addObserverRedundant").d("reason", "observerAlreadyAdded"));
    }
}

void StateSynchronizer::removeObserver(std::shared_ptr<StateSynchronizer::ObserverInterface> observer) {
    if (!observer) {
        ACSDK_ERROR(LX("removeObserverFailed").d("reason", "nullObserver"));
        return;
    }
    std::lock_guard<std::mutex> observerLock(m_observerMutex);
    m_observers.erase(observer);
}

void StateSynchronizer::shutdown() {
    std::lock_guard<std::mutex> observerLock(m_observerMutex);
    m_observers.clear();
}

void StateSynchronizer::notifyObserversLocked() {
    std::unique_lock<std::mutex> observerLock(m_observerMutex);
    auto currentObservers = m_observers;
    observerLock.unlock();
    for (auto observer : currentObservers) {
        observer->onStateChanged(m_state);
    }
}

void StateSynchronizer::messageSent(MessageRequest::Status messageStatus) {
    if (MessageRequest::Status::SUCCESS == messageStatus) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (ObserverInterface::State::SYNCHRONIZED != m_state) {
            m_state = ObserverInterface::State::SYNCHRONIZED;
            notifyObserversLocked();
        }
    } else {
        // If the message send was unsuccessful, send another request to @c ContextManager.
        ACSDK_ERROR(LX("messageSendNotSuccessful"));
        m_contextManager->getContext(shared_from_this());
    }
}

void StateSynchronizer::onConnectionStatusChanged(
        const ConnectionStatusObserverInterface::Status status,
        const ConnectionStatusObserverInterface::ChangedReason reason) {
    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (ConnectionStatusObserverInterface::Status::CONNECTED == status) {
        if (ObserverInterface::State::SYNCHRONIZED == m_state) {
            ACSDK_ERROR(LX("unexpectedConnectionStatusChange").d("reason", "connectHappenedUnexpectedly"));
        } else {
            // This is the case when we should send @c SynchronizeState event.
            m_contextManager->getContext(shared_from_this());
        }
    } else {
        if (ObserverInterface::State::NOT_SYNCHRONIZED == m_state) {
            ACSDK_INFO(LX("unexpectedConnectionStatusChange").d("reason", "noConnectHappenedUnexpectedly"));
        } else {
            // This is the case when we should notify observers that the connection is not yet synchronized.
            m_state = ObserverInterface::State::NOT_SYNCHRONIZED;
            notifyObserversLocked();
        }
    }
}

void StateSynchronizer::onContextAvailable(const std::string& jsonContext) {
    auto msgIdAndJsonEvent = buildJsonEventString(
            STATE_SYNCHRONIZER_NAMESPACE,
            STATE_SYNCHRONIZER_NAME,
            "",
            "{}",
            jsonContext);
    m_messageSender->sendMessage(
            std::make_shared<NotifyingMessageRequest>(msgIdAndJsonEvent.second, shared_from_this()));
}

void StateSynchronizer::onContextFailure(const ContextRequestError error) {
    ACSDK_ERROR(LX("contextRetrievalFailed").d("reason", "contextRequestErrorOccurred").d("error", error));
    ACSDK_DEBUG(LX("retryContextRetrieve").d("reason", "contextRetrievalFailed"));
    m_contextManager->getContext(shared_from_this());
}

StateSynchronizer::StateSynchronizer(
        std::shared_ptr<avsCommon::sdkInterfaces::ContextManagerInterface> contextManager,
        std::shared_ptr<avsCommon::sdkInterfaces::MessageSenderInterface> messageSender) :
    m_messageSender{messageSender},
    m_contextManager{contextManager},
    m_state{ObserverInterface::State::NOT_SYNCHRONIZED}
{
}

} // namespace system
} // namespace capabilityAgents
} // namespace alexaClientSDK
