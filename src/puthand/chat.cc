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

using namespace ajn;

/* constants. */
static const char* CHAT_SERVICE_INTERFACE_NAME = "org.alljoyn.bus.samples.chat";
static const char* CHAT_SERVICE_OBJECT_PATH = "/chatService";
static const SessionPort CHAT_PORT = 27;

/* static data. */
static ajn::BusAttachment* s_bus = NULL;
static qcc::String s_joinName = "org.alljoyn.bus.samples.chat.iot_hand";
static qcc::String s_sessionHost;
static SessionId s_sessionId = 0;
static bool s_joinComplete = false;
static volatile sig_atomic_t s_interrupt = false;

static void CDECL_CALL SigIntHandler(int sig)
{
    QCC_UNUSED(sig);
    s_interrupt = true;
}

/*
 * get a line of input from the the file pointer (most likely stdin).
 * This will capture the the num-1 characters or till a newline character is
 * entered.
 *
 * @param[out] str a pointer to a character array that will hold the user input
 * @param[in]  num the size of the character array 'str'
 * @param[in]  fp  the file pointer the sting will be read from. (most likely stdin)
 *
 * @return returns the same string as 'str' if there has been a read error a null
 *                 pointer will be returned and 'str' will remain unchanged.
 */
char*get_line(char*str, size_t num, FILE*fp)
{
    char*p = fgets(str, num, fp);

    // fgets will capture the '\n' character if the string entered is shorter than
    // num. Remove the '\n' from the end of the line and replace it with nul '\0'.
    if (p != NULL) {
        size_t last = strlen(str) - 1;
        if (str[last] == '\n') {
            str[last] = '\0';
        }
    }

    return s_interrupt ? NULL : p;
}

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

        if (s_sessionHost.empty()) {
            printf("Discovered chat conversation\n");

            /* Join the conversation */
            /* Since we are in a callback we must enable concurrent callbacks before calling a synchronous method. */
            s_sessionHost = name;
            s_bus->EnableConcurrentCallbacks();
            SessionOpts opts(SessionOpts::TRAFFIC_MESSAGES, true, SessionOpts::PROXIMITY_ANY, TRANSPORT_ANY);
            QStatus status = s_bus->JoinSession(name, CHAT_PORT, this, s_sessionId, opts);
            if (ER_OK == status) {
                printf("Joined conversation\n");
            } else {
                printf("JoinSession failed (status=%s)\n", QCC_StatusText(status));
            }
            uint32_t timeout = 20;
            status = s_bus->SetLinkTimeout(s_sessionId, timeout);
            if (ER_OK == status) {
                printf("Set link timeout to %d\n", timeout);
            } else {
                printf("Set link timeout failed\n");
            }
            s_joinComplete = true;
        }
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

/** Begin discovery on the well-known name of the service to be called, report the result to
   stdout, and return the result status. */
QStatus FindAdvertisedName(void)
{
    /* Begin discovery on the well-known name of the service to be called */
    QStatus status = s_bus->FindAdvertisedName(s_joinName.c_str());

    if (status == ER_OK) {
        printf("org.alljoyn.Bus.FindAdvertisedName ('%s') succeeded.\n", s_joinName.c_str());
    } else {
        printf("org.alljoyn.Bus.FindAdvertisedName ('%s') failed (%s).\n", s_joinName.c_str(), QCC_StatusText(status));
    }

    return status;
}

/** Wait for join session to complete, report the event to stdout, and return the result status. */
QStatus WaitForJoinSessionCompletion(void)
{
    unsigned int count = 0;

    while (!s_joinComplete && !s_interrupt) {
        if (0 == (count++ % 100)) {
            printf("Waited %u seconds for JoinSession completion.\n", count / 100);
        }

#ifdef _WIN32
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
    }

    return s_joinComplete && !s_interrupt ? ER_OK : ER_ALLJOYN_JOINSESSION_REPLY_CONNECT_FAILED;
}

/** Take input from stdin and send it as a chat message, continue until an error or
 * SIGINT occurs, return the result status. */
QStatus DoTheChat(void)
{
    const int bufSize = 1024;
    char buf[bufSize];
    QStatus status = ER_OK;

    while ((ER_OK == status) && (get_line(buf, bufSize, stdin))) {
        status = s_chatObj->SendChatSignal(buf);
    }

    return status;
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

    /* Install SIGINT handler. */
    signal(SIGINT, SigIntHandler);

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

        if (ER_OK == status) {
            status = FindAdvertisedName();
        }

        if (ER_OK == status) {
            status = WaitForJoinSessionCompletion();
        }

        if (ER_OK == status) {
            status = DoTheChat();
        }
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
