/*
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <alljoyn/AllJoynStd.h>
#include <alljoyn/BusAttachment.h>
#include <alljoyn/BusObject.h>
#include <alljoyn/DBusStd.h>
#include <alljoyn/Init.h>
#include <alljoyn/InterfaceDescription.h>
#include <alljoyn/ProxyBusObject.h>
#include <qcc/Log.h>
#include <qcc/String.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <iostream>
#include <cstring>
#include "Leap.h"

using namespace Leap;
using namespace ajn;

/* constants. */
static const char* CHAT_SERVICE_INTERFACE_NAME = "org.alljoyn.bus.samples.chat";
static const char* CHAT_SERVICE_OBJECT_PATH = "/chatService";
static const SessionPort CHAT_PORT = 27;

/* static data. */
static ajn::BusAttachment* s_bus = NULL;
static qcc::String s_advertisedName = "org.alljoyn.bus.samples.chat.iot_hand";
static qcc::String s_sessionHost;
static SessionId s_sessionId = 0;

/* Bus object */
class ChatObject : public BusObject {
  public:

    ChatObject(BusAttachment& bus, const char* path) : BusObject(path), chatSignalMember(NULL)
    {
        QStatus status;

        /* Add the chat interface to this object */
        const InterfaceDescription* chatIntf = bus.GetInterface(CHAT_SERVICE_INTERFACE_NAME);
        assert(chatIntf);
        AddInterface(*chatIntf);

        /* Store the Chat signal member away so it can be quickly looked up when signals are sent */
        chatSignalMember = chatIntf->GetMember("Chat");
        assert(chatSignalMember);

        /* Register signal handler */
        status =  bus.RegisterSignalHandler(this,
                                            static_cast<MessageReceiver::SignalHandler>(&ChatObject::ChatSignalHandler),
                                            chatSignalMember,
                                            NULL);

        if (ER_OK != status) {
            printf("Failed to register signal handler for ChatObject::Chat (%s)\n", QCC_StatusText(status));
        }
    }

    /** Send a Chat signal */
    QStatus SendChatSignal(const char* msg) {

        MsgArg chatArg("s", msg);
        uint8_t flags = 0;
        if (0 == s_sessionId) {
            printf("Sending Chat signal without a session id\n");
        }
        return Signal(NULL, s_sessionId, *chatSignalMember, &chatArg, 1, 0, flags);
    }

    /** Receive a signal from another Chat client */
    void ChatSignalHandler(const InterfaceDescription::Member* member, const char* srcPath, Message& msg)
    {
        QCC_UNUSED(member);
        QCC_UNUSED(srcPath);
        printf("%s: %s\n", msg->GetSender(), msg->GetArg(0)->v_string.str);
    }

	virtual void GetProp(const InterfaceDescription::Member* /*member*/, Message& /*msg*/) {}
	virtual void SetProp(const InterfaceDescription::Member* /*member*/, Message& /*msg*/) {}

  private:
    const InterfaceDescription::Member* chatSignalMember;
};

class MyBusListener : public BusListener, public SessionPortListener, public SessionListener {
    void FoundAdvertisedName(const char* name, TransportMask transport, const char* namePrefix)
    {
        printf("FoundAdvertisedName(name='%s', transport = 0x%x, prefix='%s')\n", name, transport, namePrefix);
    }
    void LostAdvertisedName(const char* name, TransportMask transport, const char* namePrefix)
    {
        QCC_UNUSED(namePrefix);
        printf("Got LostAdvertisedName for %s from transport 0x%x\n", name, transport);
    }
    void NameOwnerChanged(const char* busName, const char* previousOwner, const char* newOwner)
    {
        printf("NameOwnerChanged: name=%s, oldOwner=%s, newOwner=%s\n", busName, previousOwner ? previousOwner : "<none>",
               newOwner ? newOwner : "<none>");
    }
    bool AcceptSessionJoiner(SessionPort sessionPort, const char* joiner, const SessionOpts& opts)
    {
        if (sessionPort != CHAT_PORT) {
            printf("Rejecting join attempt on non-chat session port %d\n", sessionPort);
            return false;
        }

        printf("Accepting join session request from %s (opts.proximity=%x, opts.traffic=%x, opts.transports=%x)\n",
               joiner, opts.proximity, opts.traffic, opts.transports);
        return true;
    }

    void SessionJoined(SessionPort sessionPort, SessionId id, const char* joiner)
    {
        QCC_UNUSED(sessionPort);

        s_sessionId = id;
        printf("SessionJoined with %s (id=%d)\n", joiner, id);
        s_bus->EnableConcurrentCallbacks();
        uint32_t timeout = 20;
        QStatus status = s_bus->SetLinkTimeout(s_sessionId, timeout);
        if (ER_OK == status) {
            printf("Set link timeout to %d\n", timeout);
        } else {
            printf("Set link timeout failed\n");
        }
    }
};

/* More static data. */
static ChatObject* s_chatObj = NULL;
static MyBusListener s_busListener;

#ifdef __cplusplus
extern "C" {
#endif

/** Create the interface, report the result to stdout, and return the result status. */
QStatus CreateInterface(void)
{
    /* Create org.alljoyn.bus.samples.chat interface */
    InterfaceDescription* chatIntf = NULL;
    QStatus status = s_bus->CreateInterface(CHAT_SERVICE_INTERFACE_NAME, chatIntf);

    if (ER_OK == status) {
        chatIntf->AddSignal("Chat", "s",  "str", 0);
        chatIntf->Activate();
    } else {
        printf("Failed to create interface \"%s\" (%s)\n", CHAT_SERVICE_INTERFACE_NAME, QCC_StatusText(status));
    }

    return status;
}

/** Start the message bus, report the result to stdout, and return the status code. */
QStatus StartMessageBus(void)
{
    QStatus status = s_bus->Start();

    if (ER_OK == status) {
        printf("BusAttachment started.\n");
    } else {
        printf("Start of BusAttachment failed (%s).\n", QCC_StatusText(status));
    }

    return status;
}

/** Register the bus object and connect, report the result to stdout, and return the status code. */
QStatus RegisterBusObject(void)
{
    QStatus status = s_bus->RegisterBusObject(*s_chatObj);

    if (ER_OK == status) {
        printf("RegisterBusObject succeeded.\n");
    } else {
        printf("RegisterBusObject failed (%s).\n", QCC_StatusText(status));
    }

    return status;
}

/** Connect, report the result to stdout, and return the status code. */
QStatus ConnectBusAttachment(void)
{
    QStatus status = s_bus->Connect();

    if (ER_OK == status) {
        printf("Connect to '%s' succeeded.\n", s_bus->GetConnectSpec().c_str());
    } else {
        printf("Failed to connect to '%s' (%s).\n", s_bus->GetConnectSpec().c_str(), QCC_StatusText(status));
    }

    return status;
}

/** Request the service name, report the result to stdout, and return the status code. */
QStatus RequestName(void)
{
    QStatus status = s_bus->RequestName(s_advertisedName.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE);

    if (ER_OK == status) {
        printf("RequestName('%s') succeeded.\n", s_advertisedName.c_str());
    } else {
        printf("RequestName('%s') failed (status=%s).\n", s_advertisedName.c_str(), QCC_StatusText(status));
    }

    return status;
}

/** Create the session, report the result to stdout, and return the status code. */
QStatus CreateSession(TransportMask mask)
{
    SessionOpts opts(SessionOpts::TRAFFIC_MESSAGES, true, SessionOpts::PROXIMITY_ANY, mask);
    SessionPort sp = CHAT_PORT;
    QStatus status = s_bus->BindSessionPort(sp, opts, s_busListener);

    if (ER_OK == status) {
        printf("BindSessionPort succeeded.\n");
    } else {
        printf("BindSessionPort failed (%s).\n", QCC_StatusText(status));
    }

    return status;
}

/** Advertise the service name, report the result to stdout, and return the status code. */
QStatus AdvertiseName(TransportMask mask)
{
    QStatus status = s_bus->AdvertiseName(s_advertisedName.c_str(), mask);

    if (ER_OK == status) {
        printf("Advertisement of the service name '%s' succeeded.\n", s_advertisedName.c_str());
    } else {
        printf("Failed to advertise name '%s' (%s).\n", s_advertisedName.c_str(), QCC_StatusText(status));
    }

    return status;
}

class SampleListener : public Listener {
public:
	virtual void onInit(const Controller&);
	virtual void onConnect(const Controller&);
	virtual void onDisconnect(const Controller&);
	virtual void onExit(const Controller&);
	virtual void onFrame(const Controller&);
	virtual void onFocusGained(const Controller&);
	virtual void onFocusLost(const Controller&);
	virtual void onDeviceChange(const Controller&);
	virtual void onServiceConnect(const Controller&);
	virtual void onServiceDisconnect(const Controller&);
	virtual void onServiceChange(const Controller&);
	virtual void onDeviceFailure(const Controller&);
	virtual void onLogMessage(const Controller&, MessageSeverity severity, int64_t timestamp, const char* msg);
};

const std::string fingerNames[] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
const std::string boneNames[] = { "Metacarpal", "Proximal", "Middle", "Distal" };

void SampleListener::onInit(const Controller& controller) {
	std::cout << "Initialized" << std::endl;
}

void SampleListener::onConnect(const Controller& controller) {
	std::cout << "Connected" << std::endl;
}

void SampleListener::onDisconnect(const Controller& controller) {
	// Note: not dispatched when running in a debugger.
	std::cout << "Disconnected" << std::endl;
}

void SampleListener::onExit(const Controller& controller) {
	std::cout << "Exited" << std::endl;
}

void SampleListener::onFrame(const Controller& controller) {
	std::string s = "[";

	HandList hands = controller.frame().hands();
	for (HandList::const_iterator hl = hands.begin(); hl != hands.end(); ++hl) {
		const FingerList fingers = (*hl).fingers();

		s += "[";

		for (FingerList::const_iterator fl = fingers.begin(); fl != fingers.end(); ++fl) {
			s += "[";
			for (int b = 0; b < 4; ++b) {
				Bone::Type boneType = static_cast<Bone::Type>(b);
				Bone bone = (*fl).bone(boneType);
				s += "[\"";
				s += bone.direction().toString();
				s += "\"],";
			}
			s += "],";
		}
		s += "],";
	}

	s += "]";


	s_chatObj->SendChatSignal(s.data());
}

void SampleListener::onFocusGained(const Controller& controller) {
	std::cout << "Focus Gained" << std::endl;
}

void SampleListener::onFocusLost(const Controller& controller) {
	std::cout << "Focus Lost" << std::endl;
}

void SampleListener::onDeviceChange(const Controller& controller) {
	std::cout << "Device Changed" << std::endl;
	const DeviceList devices = controller.devices();

	for (int i = 0; i < devices.count(); ++i) {
		std::cout << "id: " << devices[i].toString() << std::endl;
		std::cout << "  isStreaming: " << (devices[i].isStreaming() ? "true" : "false") << std::endl;
		std::cout << "  isSmudged:" << (devices[i].isSmudged() ? "true" : "false") << std::endl;
		std::cout << "  isLightingBad:" << (devices[i].isLightingBad() ? "true" : "false") << std::endl;
	}
}

void SampleListener::onServiceConnect(const Controller& controller) {
	std::cout << "Service Connected" << std::endl;
}

void SampleListener::onServiceDisconnect(const Controller& controller) {
	std::cout << "Service Disconnected" << std::endl;
}

void SampleListener::onServiceChange(const Controller& controller) {
	std::cout << "Service Changed" << std::endl;
}

void SampleListener::onDeviceFailure(const Controller& controller) {
	std::cout << "Device Error" << std::endl;
	const Leap::FailedDeviceList devices = controller.failedDevices();

	for (FailedDeviceList::const_iterator dl = devices.begin(); dl != devices.end(); ++dl) {
		const FailedDevice device = *dl;
		std::cout << "  PNP ID:" << device.pnpId();
		std::cout << "    Failure type:" << device.failure();
	}
}

void SampleListener::onLogMessage(const Controller&, MessageSeverity s, int64_t t, const char* msg) {
	switch (s) {
	case Leap::MESSAGE_CRITICAL:
		std::cout << "[Critical]";
		break;
	case Leap::MESSAGE_WARNING:
		std::cout << "[Warning]";
		break;
	case Leap::MESSAGE_INFORMATION:
		std::cout << "[Info]";
		break;
	case Leap::MESSAGE_UNKNOWN:
		std::cout << "[Unknown]";
	}
	std::cout << "[" << t << "] ";
	std::cout << msg << std::endl;
}

int CDECL_CALL main(int argc, char** argv)
{
    if (AllJoynInit() != ER_OK) {
        return 1;
    }
#ifdef ROUTER
    if (AllJoynRouterInit() != ER_OK) {
        AllJoynShutdown();
        return 1;
    }
#endif

    QStatus status = ER_OK;

    /* Create message bus */
    s_bus = new BusAttachment("chat", true);

    if (s_bus) {
        if (ER_OK == status) {
            status = CreateInterface();
        }

        if (ER_OK == status) {
            s_bus->RegisterBusListener(s_busListener);
        }

        if (ER_OK == status) {
            status = StartMessageBus();
        }

        /* Create the bus object that will be used to send and receive signals */
        ChatObject chatObj(*s_bus, CHAT_SERVICE_OBJECT_PATH);

        s_chatObj = &chatObj;

        if (ER_OK == status) {
            status = RegisterBusObject();
        }

        if (ER_OK == status) {
            status = ConnectBusAttachment();
        }

        /*
            * Advertise this service on the bus.
            * There are three steps to advertising this service on the bus.
            * 1) Request a well-known name that will be used by the client to discover
            *    this service.
            * 2) Create a session.
            * 3) Advertise the well-known name.
            */
        if (ER_OK == status) {
            status = RequestName();
        }

        const TransportMask SERVICE_TRANSPORT_TYPE = TRANSPORT_ANY;

        if (ER_OK == status) {
            status = CreateSession(SERVICE_TRANSPORT_TYPE);
        }

        if (ER_OK == status) {
            status = AdvertiseName(SERVICE_TRANSPORT_TYPE);
        }

		// Create a sample listener and controller
		SampleListener listener;
		Controller controller;

		// Have the sample listener receive events from the controller
		controller.addListener(listener);

	//	if (argc > 1 && strcmp(argv[1], "--bg") == 0)
			controller.setPolicy(Leap::Controller::POLICY_BACKGROUND_FRAMES);

		controller.setPolicy(Leap::Controller::POLICY_ALLOW_PAUSE_RESUME);

		// Keep this process running until Enter is pressed
		std::cout << "Press Enter to quit, or enter 'p' to pause or unpause the service..." << std::endl;

		bool paused = false;
		while (true) {
			char c = std::cin.get();
			if (c == 'p') {
				paused = !paused;
				controller.setPaused(paused);
				std::cin.get(); //skip the newline
			}
			else
				break;
		}

		// Remove the sample listener when done
		controller.removeListener(listener);

    } else {
        status = ER_OUT_OF_MEMORY;
    }

    /* Cleanup */
    delete s_bus;
    s_bus = NULL;

    printf("Chat exiting with status 0x%04x (%s).\n", status, QCC_StatusText(status));

#ifdef ROUTER
    AllJoynRouterShutdown();
#endif
    AllJoynShutdown();
    return (int) status;
}

#ifdef __cplusplus
}
#endif
