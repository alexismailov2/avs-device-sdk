/*
 * AlexaDirectiveSequencerLibraryTest.cpp
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

/// @file AlexaDirectiveSequencerLibraryTest.cpp
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <future>
#include <fstream>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "ACL/AVSConnectionManager.h"
#include "ACL/Transport/HTTP2MessageRouter.h"
#include "AVSCommon/AVS/BlockingPolicy.h"
#include "ADSL/DirectiveSequencer.h"
#include "ADSL/MessageInterpreter.h"
#include "AuthDelegate/AuthDelegate.h"
#include "AVSCommon/AVS/Attachment/AttachmentManager.h"
#include "AVSCommon/AVS/Attachment/InProcessAttachmentWriter.h"
#include "AVSCommon/AVS/Attachment/InProcessAttachmentReader.h"
#include "AVSCommon/SDKInterfaces/ExceptionEncounteredSenderInterface.h"
#include "AVSCommon/SDKInterfaces/DirectiveHandlerInterface.h"
#include "AVSCommon/SDKInterfaces/DirectiveHandlerResultInterface.h"
#include "AVSCommon/Utils/JSON/JSONUtils.h"
#include "AVSCommon/AVS/Initialization/AlexaClientSDKInit.h"
#include "AVSCommon/Utils/Logger/Logger.h"
#include "Integration/AuthObserver.h"
#include "Integration/ClientMessageHandler.h"
#include "Integration/ConnectionStatusObserver.h"
#include "Integration/ObservableMessageRequest.h"
#include "Integration/TestDirectiveHandler.h"
#include "Integration/TestExceptionEncounteredSender.h"

namespace alexaClientSDK {
namespace integration {
namespace test {

using namespace acl;
using namespace adsl;
using namespace authDelegate;
using namespace avsCommon;
using namespace avsCommon::avs;
using namespace avsCommon::sdkInterfaces;
using namespace avsCommon::avs::initialization;
using namespace avsCommon::avs::attachment;
using namespace avsCommon::utils::sds;
using namespace avsCommon::utils::json;

/// String to identify log entries originating from this file.
static const std::string TAG("AlexaDirectiveSequencerLibraryTest");

/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/**
 * This string specifies a Recognize event using the specified profile.
 *
 * CLOSE_TALK performs end-of-speech detection on the client, so no directive is sent from AVS to stop recording.
 * NEAR_FIELD performs end-of-speech detection in AVS, so a directive is sent from AVS to stop recording.
 */
#define RECOGNIZE_EVENT_JSON(PROFILE, DIALOG_REQUEST_ID )         \
    "{"                                                           \
        "\"event\":{"                                             \
            "\"payload\":{"                                       \
                "\"format\":\"AUDIO_L16_RATE_16000_CHANNELS_1\"," \
                "\"profile\":\"" #PROFILE "\""                    \
            "},"                                                  \
            "\"header\":{"                                        \
                "\"dialogRequestId\":\"" DIALOG_REQUEST_ID "\","  \
                "\"messageId\":\"messageId123\","                 \
                "\"name\":\"Recognize\","                         \
                "\"namespace\":\"SpeechRecognizer\""              \
            "}"                                                   \
        "},"                                                      \
        "\"context\":[{"                                          \
            "\"payload\":{"                                       \
                "\"activeAlerts\":[],"                            \
                "\"allAlerts\":[]"                                \
            "},"                                                  \
            "\"header\":{"                                        \
                "\"name\":\"AlertsState\","                       \
                "\"namespace\":\"Alerts\""                        \
            "}"                                                   \
        "},"                                                      \
        "{"                                                       \
            "\"payload\":{"                                       \
                "\"playerActivity\":\"IDLE\","                    \
                "\"offsetInMilliseconds\":0,"                     \
                "\"token\":\"\""                                  \
            "},"                                                  \
            "\"header\":{"                                        \
                "\"name\":\"PlaybackState\","                     \
                "\"namespace\":\"AudioPlayer\""                   \
            "}"                                                   \
        "},"                                                      \
        "{"                                                       \
            "\"payload\":{"                                       \
                "\"muted\":false,"                                \
                "\"volume\":0"                                    \
            "},"                                                  \
            "\"header\":{"                                        \
                "\"name\":\"VolumeState\","                       \
                "\"namespace\":\"Speaker\""                       \
            "}"                                                   \
        "},"                                                      \
        "{"                                                       \
            "\"payload\":{"                                       \
                "\"playerActivity\":\"FINISHED\","                \
                "\"offsetInMilliseconds\":0,"                     \
                "\"token\":\"\""                                  \
            "},"                                                  \
            "\"header\":{"                                        \
                "\"name\":\"SpeechState\","                       \
                "\"namespace\":\"SpeechSynthesizer\""             \
            "}"                                                   \
        "}]"                                                      \
    "}"


/// This is a 16 bit 16 kHz little endian linear PCM audio file of "Joke" to be recognized.
static const std::string RECOGNIZE_JOKE_AUDIO_FILE_NAME = "/recognize_joke_test.wav";
/// This is a 16 bit 16 kHz little endian linear PCM audio file of "Wikipedia" to be recognized.
static const std::string RECOGNIZE_WIKI_AUDIO_FILE_NAME = "/recognize_wiki_test.wav";
/// This is a 16 bit 16 kHz little endian linear PCM audio file of "Lions" to be recognized.
static const std::string RECOGNIZE_LIONS_AUDIO_FILE_NAME = "/recognize_lions_test.wav";
/// This is a 16 bit 16 kHz little endian linear PCM audio file of "What's up" to be recognized.
static const std::string RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME = "/recognize_whats_up_test.wav";

//String to be used as a basic DialogRequestID.
#define FIRST_DIALOG_REQUEST_ID "DialogRequestID123"
//String to be used as a DialogRequestID when the first has already been used.
#define SECOND_DIALOG_REQUEST_ID "DialogRequestID456"

/// This string specifies a Recognize event using the CLOSE_TALK profile and uses the first DialogRequestID.
static const std::string CT_FIRST_RECOGNIZE_EVENT_JSON = RECOGNIZE_EVENT_JSON(CLOSE_TALK, FIRST_DIALOG_REQUEST_ID);
/// This string specifies a Recognize event using the CLOSE_TALK profile and uses the first DialogRequestID.
static const std::string CT_FIRST_RECOGNIZE_EVENT_JSON_NEAR = RECOGNIZE_EVENT_JSON(NEAR_FIELD, FIRST_DIALOG_REQUEST_ID);
/// This string specifies a Recognize event using the CLOSE_TALK profile and uses the second DialogRequestID.
static const std::string CT_SECOND_RECOGNIZE_EVENT_JSON = RECOGNIZE_EVENT_JSON(CLOSE_TALK, SECOND_DIALOG_REQUEST_ID);

// This string to be used for ClearQueue Directives which use the NAMESPACE_AUDIO_PLAYER namespace.
static const std::string NAME_CLEAR_QUEUE = "ClearQueue";
// This string to be used for ExpectSpeech Directives which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_EXPECT_SPEECH = "ExpectSpeech";
// This string to be used for Play Directives which use the NAMESPACE_AUDIO_PLAYER namespace.
static const std::string NAME_PLAY = "Play";
// This string to be used for SetMute Directives which use the NAMESPACE_SPEAKER namespace.
static const std::string NAME_SET_MUTE = "SetMute";
// This string to be used for Speak Directives which use the NAMESPACE_SPEECH_SYNTHESIZER namespace.
static const std::string NAME_SPEAK = "Speak";
// This string to be used for Stop Directives which use the NAMESPACE_AUDIO_PLAYER namespace.
static const std::string NAME_STOP = "Stop";

// This String to be used to register the AudioPlayer namespace to a DirectiveHandler.
static const std::string NAMESPACE_AUDIO_PLAYER = "AudioPlayer";
// This String to be used to register the Speaker namespace to a DirectiveHandler.
static const std::string NAMESPACE_SPEAKER = "Speaker";
// This String to be used to register the SpeechRecognizer namespace to a DirectiveHandler.
static const std::string NAMESPACE_SPEECH_RECOGNIZER = "SpeechRecognizer";
// This String to be used to register the SpeechSynthesizer namespace to a DirectiveHandler.
static const std::string NAMESPACE_SPEECH_SYNTHESIZER = "SpeechSynthesizer";
// This string to be used for StopCapture Directives which use the NAMESPACE_SPEECH_RECOGNIZER namespace.
static const std::string NAME_STOP_CAPTURE = "StopCapture";

// This pair connects a ExpectSpeech name and SpeechRecognizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName EXPECT_SPEECH_PAIR(NAMESPACE_SPEECH_RECOGNIZER, NAME_EXPECT_SPEECH);
// This pair connects a SetMute name and Speaker namespace for use in DirectiveHandler registration.
static const NamespaceAndName SET_MUTE_PAIR(NAMESPACE_SPEAKER, NAME_SET_MUTE);
// This pair connects a Speak name and SpeechSynthesizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName SPEAK_PAIR(NAMESPACE_SPEECH_SYNTHESIZER, NAME_SPEAK);
// This pair connects a StopCapture name and SpeechRecognizer namespace for use in DirectiveHandler registration.
static const NamespaceAndName STOP_CAPTURE_PAIR(NAMESPACE_SPEECH_RECOGNIZER, NAME_STOP_CAPTURE);

// This Integer to be used to specify a timeout in seconds for a directive to reach the DirectiveHandler.
static const std::chrono::seconds WAIT_FOR_TIMEOUT_DURATION(5);
// This Integer to be used to specify a timeout in seconds for AuthDelegate to wait for LWA response.
static const std::chrono::seconds SEND_EVENT_TIMEOUT_DURATION(20);

/// JSON key to get the directive object of a message.
static const std::string JSON_MESSAGE_DIRECTIVE_KEY = "directive";
/// JSON key to get the header object of a message.
static const std::string JSON_MESSAGE_HEADER_KEY = "header";
/// JSON key to get the namespace value of a header.
static const std::string JSON_MESSAGE_NAMESPACE_KEY = "namespace";
/// JSON key to get the name value of a header.
static const std::string JSON_MESSAGE_NAME_KEY = "name";
/// JSON key to get the messageId value of a header.
static const std::string JSON_MESSAGE_MESSAGE_ID_KEY = "messageId";
/// JSON key to get the dialogRequestId value of a header.
static const std::string JSON_MESSAGE_DIALOG_REQUEST_ID_KEY = "dialogRequestId";
/// JSON key to get the payload object of a message.
static const std::string JSON_MESSAGE_PAYLOAD_KEY = "payload";

/// Path to configuration file (from command line arguments).
std::string configPath;
/// Path to directory containing input data (from command line arguments).
std::string inputPath;

class AlexaDirectiveSequencerLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::ifstream infile(configPath);
        ASSERT_TRUE(infile.good());
        ASSERT_TRUE(AlexaClientSDKInit::initialize({&infile}));
        m_authObserver = std::make_shared<AuthObserver>();
        m_authDelegate = AuthDelegate::create();
        m_authDelegate->addAuthObserver(m_authObserver);
        m_attachmentManager = std::make_shared<avsCommon::avs::attachment::AttachmentManager>(
                 AttachmentManager::AttachmentType::IN_PROCESS);
        m_connectionStatusObserver = std::make_shared<ConnectionStatusObserver>();
        m_clientMessageHandler = std::make_shared<ClientMessageHandler>(m_attachmentManager);
        bool isEnabled = false;
        m_messageRouter = std::make_shared<HTTP2MessageRouter>(m_authDelegate, m_attachmentManager);
        m_exceptionEncounteredSender = std::make_shared<TestExceptionEncounteredSender>();
        m_directiveSequencer = DirectiveSequencer::create(m_exceptionEncounteredSender);
        m_messageInterpreter = std::make_shared<MessageInterpreter>(
            m_exceptionEncounteredSender,
            m_directiveSequencer,
            m_attachmentManager);

        // note: No DirectiveHandlers have been registered with the DirectiveSequencer yet. Registration of
        // handlers is deferred to individual test implementations.

        m_avsConnectionManager = AVSConnectionManager::create(
                m_messageRouter,
                isEnabled,
                { m_connectionStatusObserver },
                { m_messageInterpreter });
        // TODO: ACSDK-421: Remove the callback when m_avsConnection manager is no longer an observer to
        // StateSynchronizer.
        m_avsConnectionManager->onStateChanged(StateSynchronizerObserverInterface::State::SYNCHRONIZED);
        connect();
    }

    void TearDown() override {
        disconnect();
        m_directiveSequencer->shutdown();
        AlexaClientSDKInit::uninitialize();
    }

    /**
     * Connect to AVS.
     */
    void connect() {
        ASSERT_TRUE(m_authObserver->waitFor(AuthObserver::State::REFRESHED))
                << "Retrieving the auth token timed out.";
        m_avsConnectionManager->enable();
        ASSERT_TRUE(m_connectionStatusObserver->waitFor(ConnectionStatusObserverInterface::Status::CONNECTED))
                << "Connecting timed out.";
    }

    /**
     * Disconnect from AVS.
     */
    void disconnect() {
        m_avsConnectionManager->disable();
        ASSERT_TRUE(m_connectionStatusObserver->waitFor(ConnectionStatusObserverInterface::Status::DISCONNECTED))
                << "Connecting timed out.";
    }

    /**
     * Send and event to AVS. Blocks until a status is received.
     *
     * @param message The message to send.
     * @param expectStatus The status to expect from the call to send the message.
     * @param timeout How long to wait for a result from delivering the message.
     */
    void sendEvent(const std::string & jsonContent,
                   std::shared_ptr<avsCommon::avs::attachment::AttachmentReader> attachmentReader,
                   MessageRequest::Status expectedStatus,
                   std::chrono::seconds timeout) {
        auto messageRequest = std::make_shared<ObservableMessageRequest>(jsonContent, attachmentReader);
        m_avsConnectionManager->sendMessage(messageRequest);
        ASSERT_TRUE(messageRequest->waitFor(expectedStatus, timeout));
    }

    /**
     * Function to setup a message and send it to AVS.
     *
     * @param json A JSON string containing the message to send.
     * @param expectStatus The status to expect from the call to send the message.
     * @param timeout How long to wait for a result from delivering the message.
     */
    void setupMessageAndSend(
            const std::string& json,
            avsCommon::avs::MessageRequest::Status expectedStatus,
            std::chrono::seconds timeout) {
        sendEvent(json, nullptr, expectedStatus, timeout);
    }

    /**
     * Function to setup a message with an attachment and send it to AVS.
     *
     * @param json A JSON string containing the message to send.
     * @param file Name of the file to read the attachment from.
     * @param expectStatus The status to expect from the call to send the message.
     * @param timeout How long to wait for a result from delivering the message.
     */
    void setupMessageWithAttachmentAndSend(
            const std::string& json,
            std::string& file,
            avsCommon::avs::MessageRequest::Status expectedStatus,
            std::chrono::seconds timeout) {

        auto is = std::make_shared<std::ifstream>(file, std::ios::binary);
        ASSERT_TRUE(is->is_open());

        const int mbBytes = 1024 * 1024;

        std::vector<char> localBuffer(mbBytes);

        auto bufferSize = InProcessSDS::calculateBufferSize(localBuffer.size());
        auto buffer = std::make_shared<InProcessSDSTraits::Buffer>(bufferSize);
        std::shared_ptr<InProcessSDS> sds = InProcessSDS::create(buffer);

        auto attachmentWriter = InProcessAttachmentWriter::create(sds);

        while (*is) {
            is->read(localBuffer.data(), mbBytes);
            size_t numBytesRead = is->gcount();
            AttachmentWriter::WriteStatus writeStatus = AttachmentWriter::WriteStatus::OK;
            attachmentWriter->write(localBuffer.data(), numBytesRead, &writeStatus);

            // write status should be either OK or CLOSED
            bool writeStatusOk = (AttachmentWriter::WriteStatus::OK == writeStatus ||
                    AttachmentWriter::WriteStatus::CLOSED == writeStatus);
            ASSERT_TRUE(writeStatusOk);
        }

        attachmentWriter->close();

        std::shared_ptr<InProcessAttachmentReader> attachmentReader =
                InProcessAttachmentReader::create(AttachmentReader::Policy::NON_BLOCKING, sds);
        ASSERT_NE(attachmentReader, nullptr);

        sendEvent(json, attachmentReader, expectedStatus, std::chrono::seconds(timeout));
    }

    /**
     * Helper function to check that a directive with the given name is an exception.
     *
     * @param Pointer to a exception encountered sender to receive exceptions through.
     * @param String of the name of the directive that should be an exception.
     */
    void assertExceptionWithName(const std::string name) {
        TestExceptionEncounteredSender::ExceptionParams params;
        bool nameFound = false;
        do {
            params = m_exceptionEncounteredSender->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
            if (params.directive->getName() == name) {
                nameFound = true;
            }
        } while (params.type != TestExceptionEncounteredSender::ExceptionParams::Type::TIMEOUT
                && !nameFound);
        ASSERT_TRUE(nameFound);
        ASSERT_NE (params.type, TestExceptionEncounteredSender::ExceptionParams::Type::TIMEOUT);
    }

    /// Object to monitor the status of the authorization to communicate with @c AVS.
    std::shared_ptr<AuthObserver> m_authObserver;

    /// Object to acquire authorization to communicate with @c AVS.
    std::shared_ptr<AuthDelegate> m_authDelegate;

    /// The Attachment Manager.
    std::shared_ptr<avsCommon::avs::attachment::AttachmentManager> m_attachmentManager;

    /// Object to monitor the status of the connection with @c AVS.
    std::shared_ptr<ConnectionStatusObserver> m_connectionStatusObserver;

    /// Object that routs messages from @c AVS.
    std::shared_ptr<MessageRouter> m_messageRouter;

    /// Object that manages the connection to @c AVS.
    std::shared_ptr<AVSConnectionManager> m_avsConnectionManager;

    /// The @c DirectiveSequencer instance to test.
    std::shared_ptr<DirectiveSequencerInterface> m_directiveSequencer;

    /// Object to convert messages from @c AVS in to directives passed to the @c DirectiveSequencer.
    std::shared_ptr<MessageInterpreter> m_messageInterpreter;

    /// Object to proxy messages from AVS to the @c MessageInterpreter.
    std::shared_ptr<ClientMessageHandler> m_clientMessageHandler;

    std::shared_ptr<TestExceptionEncounteredSender> m_exceptionEncounteredSender;
};



/**
 * Test DirectiveSequencer's ability to pass an @c AVSDirective to a @c DirectiveHandler.
 *
 * This test is intended to test @c DirectiveSequencer's ability to pass an @c AVSDirective to a @c DirectiveHandler
 * that has been registered to handle an @c AVSDirective.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, sendEventWithDirective) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will prompt SetMute and Speak.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Wait for the first directive to route through to our handler.
    auto params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_FALSE(params.isTimeout());

}

/**
 * Test @c DirectiveSequencer's ability to pass a group of no-blocking @c AVSDirectives to a @c DirectiveHandler.
 *
 * This test registers @c NON_BLOCKING handling for a suite of directives expected in response to recognize
 * request. It then verifies that handleDirective() is called for the subsequent directives without waiting for
 * completion of handling of any of the directives.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, sendDirectiveGroupWithoutBlocking) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will prompt SetMute and Speak.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Look for SetMute and Speak without completing the handling of any directives.
    TestDirectiveHandler::DirectiveParams params;
    TestDirectiveHandler::DirectiveParams setMuteParams;
    TestDirectiveHandler::DirectiveParams speakParams;
    while (true) {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        if (params.isTimeout()) {
            break;
        }
        if (params.directive->getName() == NAME_SET_MUTE) {
            setMuteParams = params;
        } else if (params.directive->getName() == NAME_SPEAK) {
            speakParams = params;
        }
    }
    ASSERT_TRUE(setMuteParams.isHandle());
    ASSERT_TRUE(speakParams.isHandle());
}

/**
 * Test @c DirectiveSequencer's ability to drop directives that do not match the current @c dialogRequestId.
 *
 * The test first sets the @c dialogRequestId, sends an event with that @c dialopRequestId, flushes the resulting
 * directives, then (without updating the current @c dialogRequestId) sends an event with a new @c dialogRequestId.
 * It then verifies that the directive handler was not called for the @c AVSDirectives expected to result from the
 * second event.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, sendDirectiveWithDifferentDialogRequestID) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio for a flashbriefing which will send back at least SetMute, Speak, SetMute, Play and Play.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Drain the directive results until we get a timeout. There should be no cancels or exceptions.
    TestDirectiveHandler::DirectiveParams params;
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isCancel());
    } while (!params.isTimeout());

    // Send an event that has a different dialogRequestID, without calling setDialogRequestId().
    file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_SECOND_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Directives from the second event do not reach the directive handler because they do no have
    // the current dialogRequestId.
    params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_TRUE(params.isTimeout());
}

/**
 * Test @c DirectiveSequencer's ability to drop queued @c AVSDirectives after Barge-In.
 *
 * This test registers handlers, including a blocking handler for @c AVSDirectives known to come from a canned
 * @c Recognize event. It then consumes the handling events up to the point of handling the blocking @c AVSDirective.
 * Then the @c dialogRequestId is changed (canceling the blocking @c AVSDirective and any subsequent @c AVSDirectives
 * in that group. Finally, a new @c Recognize event with the new @c dialogRequestId is sent. The events
 * are then consumed verifying cancellation of @c AVSDirectives from the first group and handling of @c AVSDirectives
 * in the second group.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, dropQueueAfterBargeIn) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio for a flashbriefing which will send back (at least) SetMute, Speak, SetMute, Play and Play.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    TestDirectiveHandler::DirectiveParams params;

    // Consume up to the blocking directive.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isTimeout());
    } while (!params.isHandle() || params.directive->getName() != NAME_SPEAK);
    ASSERT_EQ(params.directive->getDialogRequestId(), FIRST_DIALOG_REQUEST_ID);

    // Call setDialogRequestId(), canceling the previous group. Then send a new event with the new dialogRequestId.
    m_directiveSequencer->setDialogRequestId(SECOND_DIALOG_REQUEST_ID);
    std::string differentFile = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
            CT_SECOND_RECOGNIZE_EVENT_JSON,
            differentFile,
            avsCommon::avs::MessageRequest::Status::SUCCESS,
            SEND_EVENT_TIMEOUT_DURATION);

    // Consume cancellations and the new directives.
    bool cancelCalled = false;
    bool handleCalled = false;
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        if (params.isCancel()) {
            ASSERT_EQ(params.directive->getDialogRequestId(), FIRST_DIALOG_REQUEST_ID);
            cancelCalled = true;
        } else if (params.isHandle()) {
            ASSERT_EQ(params.directive->getDialogRequestId(), SECOND_DIALOG_REQUEST_ID);
            ASSERT_TRUE(params.result);
            params.result->setCompleted();
            handleCalled = true;
        }
    } while (!params.isTimeout());
    ASSERT_TRUE(cancelCalled);
    ASSERT_TRUE(handleCalled);
}

/**
 * Test @c DirectiveSequencer's ability to handle a Directive without a DialogRequestID.
 *
 * This test sends a @c NEAR_FIELD @c Recognize event to AVS to trigger delivery of a @c StopCapture directive.
 * @c StopCapture directives do not have a @c dialogRequestId value. This test uses that fact to verify that
 * @c AVSDirectives with no @c dialogRequestId are processed properly.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, sendDirectiveWithoutADialogRequestID) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[STOP_CAPTURE_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will prompt SetMute and Speak.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON_NEAR,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    TestDirectiveHandler::DirectiveParams params;

    // Make sure we get preHandle followed by handle for StopCapture.
    
    params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_TRUE(params.isPreHandle());
    ASSERT_TRUE(params.directive->getDialogRequestId().empty());
    ASSERT_EQ(params.directive->getName(), NAME_STOP_CAPTURE);

    params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_TRUE(params.isHandle());
    ASSERT_TRUE(params.directive->getDialogRequestId().empty());
    ASSERT_EQ(params.directive->getName(), NAME_STOP_CAPTURE);

    params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    while (!params.isTimeout()) {
        // Make sure no other calls for StopCapture are made except for the initial handleImmediately.
        ASSERT_NE(params.directive->getName(), NAME_STOP_CAPTURE);
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    }
}

/**
 * Test @c DirectiveSequencer's ability to make both preHandleDirective() and handleDirective() call for
 * @c AVSDirectives with a non-empty @c dialogRequestId.
 *
 * This test registers handlers for the directives expected in response to a @c Recognize event. It then counts the
 * number of @c preHandleDirective() and @c handleDirective() callbacks verifying that the counts come out to the
 * same value in the end.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, sendDirectivesForPreHandling) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio for a flashbriefing which will send back SetMute, Speak, SetMute, Play and Play.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Count each preHandle and handle that arrives.
    TestDirectiveHandler::DirectiveParams params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    int preHandleCounter = 0;
    int onHandleCounter = 0;
    while (!params.isTimeout()) {
        if (params.isPreHandle()) {
            ++preHandleCounter;
        } else if (params.isHandle()) {
            ++onHandleCounter;
            ASSERT_TRUE(params.result);
            params.result->setCompleted();
        }
        ASSERT_TRUE(preHandleCounter >= onHandleCounter);
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    }
    // Verify there were the same number of calls for each.
    ASSERT_EQ(preHandleCounter, onHandleCounter);
}

/**
 * Test @c DirectiveSequencer's ability to drop the head of a @c dialogRequestId group.
 *
 * This test registers handlers (including a blocking handler) for the @c AVSDirectives expected in response to a
 * canned @c Recognize request. When @c handleDirective() is called for the blocking @c AVSDirective, setFailed()
 * is called to trigger the cancellation of subsequent @c AVSDirectives in the same group.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, cancelDirectivesWhileInQueue) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio for a flashbriefing which will send back (at least) SetMute, Speak, SetMute, Play, and Play.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    TestDirectiveHandler::DirectiveParams params;

    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isTimeout());
    } while (!params.isHandle() || params.directive->getName() != NAME_SPEAK);

    // Send back error for the speak handler.
    ASSERT_TRUE(params.result);
    params.result->setFailed("Test Error");

    // Check that no other directives arrive for handling.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    } while (params.isCancel() || params.isPreHandle());
    ASSERT_TRUE(params.isTimeout());
}

/**
 * Test @c DirectiveSequencer's ability to sequence a group that has a Blocking Directive before other directives.
 *
 * This test is intended to verify the Directive Sequencer's ability to handle a dialogRequestID group that has a
 * blocking directive, followed by non-blocking directives. Expect that the directive handler will receive a SetMute
 * directive and then nothing until setComplete() is called for that directive. Then expect the directive handler
 * to receive at least one subsequent directive.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, oneBlockingDirectiveAtTheFront) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will prompt a stream of directives including SetMute.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Expect set-mute which is blocking and no other handles after that (timeout reached because SetMute blocks).
    TestDirectiveHandler::DirectiveParams params;
    TestDirectiveHandler::DirectiveParams blockingParams;
    while (true) {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        if (params.isTimeout()) {
            break;
        }
        if (params.isHandle()) {
            if (params.directive->getName() == NAME_SET_MUTE) {
                // Note the blocking params from handle so we can unblock below.
                blockingParams = params;
            } else {
                ASSERT_FALSE(blockingParams.isHandle());
            }
        }
    }
    ASSERT_TRUE(blockingParams.isHandle());

    // Unblock the queue.
    blockingParams.result->setCompleted();

    // Expect subsequent directives, including Speak.
    TestDirectiveHandler::DirectiveParams speakParams;
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        if (params.isHandle() && params.directive->getName() == NAME_SPEAK) {
            // Remember that we saw a speak param.
            speakParams = params;
        }
    } while (!params.isTimeout());
    ASSERT_TRUE(speakParams.isHandle());
}

/**
 * Test @c DirectiveSequencer's ability to sequence a group that has one @c BLOCKING @c AVSDirective in the middle.
 *
 * This test is intended to test the @c DirectiveSequencer's ability to handle a @c dialogRequestID group that has at
 * least one non-blocking directive, followed by a blocking directive, followed by non-blocking directives.
 * @c preHandleDirective() and @c handleDirective() should be called for directives before the Speak directive,
 * whose handling blocks further handling of directives. Once @c setComplete() is called for the @c BLOCKING
 * @c AVSDirective, @c handleDirective() should be called for the subsequent (and @c NON_BLOCKING) @c AVSDirectives
 * without waiting for the completion of any subsequent @c AVSDirectives.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, oneBlockingDirectiveInTheMiddle) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio for a flashbriefing which will send back SetMute, Speak, SetMute, Play and Play.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WHATS_UP_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    TestDirectiveHandler::DirectiveParams params;

    // Expect SetMute which is non-blocking.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isTimeout());
    } while (!params.isHandle() || params.directive->getName() != NAME_SET_MUTE);

    TestDirectiveHandler::DirectiveParams blockingParams;

    // Expect Speak which is blocking.
    do {
        blockingParams = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(blockingParams.isTimeout());
    } while (!blockingParams.isHandle() || blockingParams.directive->getName() != NAME_SPEAK);

    // Expect a timeout because we're blocked.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isHandle());
    } while (!params.isTimeout());

    // Unblock the queue.
    ASSERT_TRUE(blockingParams.result);
    blockingParams.result->setCompleted();

    // See things that were previously blocked in the queue come through afterward.
    params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_FALSE(params.isTimeout());
}

/**
 * Test @c DirectiveSequencer's ability to drop a directive when no handler is registered for it.
 *
 * To do this, no handler is set for a directive (@c SetMute) that is known to come down consistently in response to
 * a Recognize event, instead an exception encountered is expected.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, noDirectiveHandlerRegisteredForADirectiveAtTheFront) {
    // Don't Register a DirectiveHandler for SetMute.
    DirectiveHandlerConfiguration config;
    config[SPEAK_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will trigger SetMute and possibly others.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Make sure no SetMute directives are given to the handler, and that they result in exception encountered.
    assertExceptionWithName(NAME_SET_MUTE);
}

/**
 * Test @c DirectiveSequencer's ability to drop a directive in the middle when no handler is registered for it.
 *
 * To do this, no handler is set for a directive (@c SetMute) that is known to come down consistently in response to
 * a Recognize event, instead an exception encountered is expected.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, noDirectiveHandlerRegisteredForADirectiveInTheMiddle) {
    // Don't Register a DirectiveHandler for Speak.
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));


    // Send audio of "Joke" that will trigger SetMute and speak.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Make sure no Speak directives are given to the handler, and that they result in exception encountered.
    assertExceptionWithName(NAME_SPEAK);
}

/**
 * Test @c DirectiveSequencer's ability to refuse to overwrite registration of a directive handler.
 *
 * To do this, an attempt is made to set two different handlers for the same directive. The @c DirectiveSequencer
 * is expected to refuse the second handler. This directive is known to come down consistently in response to a
 * Recognize event. The Handler that was first set is the only one that should receive the directive.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, twoDirectiveHandlersRegisteredForADirective) {
    DirectiveHandlerConfiguration handlerAConfig;
    handlerAConfig[SET_MUTE_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandlerA = std::make_shared<TestDirectiveHandler>(handlerAConfig);

    DirectiveHandlerConfiguration handlerbConfig;
    handlerbConfig[SET_MUTE_PAIR] = BlockingPolicy::BLOCKING;
    auto directiveHandlerB = std::make_shared<TestDirectiveHandler>(handlerbConfig);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandlerA));

    // Attempt to overwrite one of the handlers
    ASSERT_FALSE(m_directiveSequencer->addDirectiveHandler(directiveHandlerB));

    // Send audio of "Joke" that will prompt SetMute.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // A received the SetMute directive.
    TestDirectiveHandler::DirectiveParams paramsA;
    do {
        paramsA = directiveHandlerA->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(paramsA.isTimeout());
    } while (!paramsA.isHandle() && paramsA.directive->getName() != NAME_SET_MUTE);

    // B receives nothing.
    TestDirectiveHandler::DirectiveParams paramsB = directiveHandlerB->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
    ASSERT_TRUE(paramsB.isTimeout());
}


/**
 * Test @c DirectiveSequencer's ability to handle a multi-turn scenario
 *
 * This test is intended to test the Directive Sequencer's ability to go through a full loop of sending a recognize
 * event that will prompt a multi-turn directive, receiving a directive group that contains ExpectSpeech, sending a
 * recognize event to respond to Alexa's question, and receiving the final directive group.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, multiturnScenario) {
    DirectiveHandlerConfiguration config;
    config[SET_MUTE_PAIR] = BlockingPolicy::NON_BLOCKING;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;
    config[EXPECT_SPEECH_PAIR] = BlockingPolicy::NON_BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "wikipedia" which will prompt a SetMute, a Speak, and an ExpectSpeech.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_WIKI_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_FIRST_RECOGNIZE_EVENT_JSON,
        file,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    TestDirectiveHandler::DirectiveParams params;

    // Check we're being told to ExpectSpeech.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isTimeout());
        ASSERT_EQ(params.directive->getDialogRequestId(), FIRST_DIALOG_REQUEST_ID);
        if (params.isHandle()) {
            params.result->setCompleted();
        }
    } while ( !params.isHandle() || params.directive->getName() != NAME_EXPECT_SPEECH);

    // Send back a recognize event.
    m_directiveSequencer->setDialogRequestId(SECOND_DIALOG_REQUEST_ID);
    std::string differentFile = inputPath + RECOGNIZE_LIONS_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
        CT_SECOND_RECOGNIZE_EVENT_JSON,
        differentFile,
        avsCommon::avs::MessageRequest::Status::SUCCESS,
        SEND_EVENT_TIMEOUT_DURATION);

    // Just the wikipedia directive group in response.
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        if (params.isHandle()) {
            ASSERT_EQ(params.directive->getDialogRequestId(), SECOND_DIALOG_REQUEST_ID);
            params.result->setCompleted();
        }
    } while (!params.isTimeout());
}

/**
 * Test ability to get an attachment from @c AttachmentManager.
 */
TEST_F(AlexaDirectiveSequencerLibraryTest, getAttachmentWithContentId) {
    DirectiveHandlerConfiguration config;
    config[SPEAK_PAIR] = BlockingPolicy::BLOCKING;

    auto directiveHandler = std::make_shared<TestDirectiveHandler>(config);

    ASSERT_TRUE(m_directiveSequencer->addDirectiveHandler(directiveHandler));

    // Send audio of "Joke" that will prompt SetMute and Speak.
    m_directiveSequencer->setDialogRequestId(FIRST_DIALOG_REQUEST_ID);
    std::string file = inputPath + RECOGNIZE_JOKE_AUDIO_FILE_NAME;
    setupMessageWithAttachmentAndSend(
            CT_FIRST_RECOGNIZE_EVENT_JSON,
            file,
            avsCommon::avs::MessageRequest::Status::SUCCESS,
            SEND_EVENT_TIMEOUT_DURATION);

    // Wait for the directive to route through to our handler.
    TestDirectiveHandler::DirectiveParams params;
    do {
        params = directiveHandler->waitForNext(WAIT_FOR_TIMEOUT_DURATION);
        ASSERT_FALSE(params.isTimeout());
    } while (!params.isPreHandle() || params.directive->getName() != NAME_SPEAK);

    auto directive = params.directive;
    std::string payloadUrl;
    jsonUtils::lookupStringValue(directive->getPayload(), "url", &payloadUrl);
    ASSERT_TRUE(!payloadUrl.empty());

    auto stringIndex = payloadUrl.find(":");
    ASSERT_TRUE(stringIndex != std::string::npos);
    ASSERT_TRUE(stringIndex != payloadUrl.size() - 1);

    auto contentId = payloadUrl.substr(payloadUrl.find(':') + 1);
    auto attachmentReader = directive->getAttachmentReader(contentId, AttachmentReader::Policy::BLOCKING);

    ASSERT_NE(attachmentReader, nullptr);
}

} // namespace test
} // namespace integration
} // namespace alexaClientSDK

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc < 3) {
        std::cerr << "USAGE: AlexaDirectiveSequencerLibraryTest <path_to_auth_delgate_config> <path_to_inputs_folder>"
                << std::endl;
        return 1;

    } else {
        alexaClientSDK::integration::test::configPath = std::string(argv[1]);
        alexaClientSDK::integration::test::inputPath = std::string(argv[2]);
        return RUN_ALL_TESTS();
    }
}


