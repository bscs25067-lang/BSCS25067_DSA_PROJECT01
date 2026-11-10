// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 

#include <thread>
#include <mutex>
#include <chrono>
#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <signal.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (ClientInfo). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;

        Node(T d) : data(d), next(nullptr) { }
        Node(T d, Node* n) : data(d), next(n) { }
    };
    Node* front, * rear;
    int count;

public:
    Queue(): front(nullptr), rear(nullptr), count(0) { }

    ~Queue()
    {
        while (!isEmpty())
        {
            dequeue();
        }
    }

    void enqueue(const T& val)
    {
        Node* newNode = new Node(val);

        if (rear == nullptr) front = rear = newNode;
        else
        {
            rear->next = newNode;
            rear = newNode;
        }

        count++;
    }

    T dequeue()
    {
        if (isEmpty()) throw runtime_error("Queue underflow");

        Node* oldFront = front;
        T val = oldFront->data;

        front = front->next;
        if (front == nullptr)
            rear = nullptr;

        delete oldFront;
        count--;

        return val;
    }

    bool isEmpty() const { return count == 0 }
    int size() const { return count; }
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    size_t totalSent = 0;
    while (totalSent < n)
    {
        ssize_t s = send(sock, buf + totalSent, n - totalSent, MSG_NOSIGNAL);

        if (s <= 0)
            return false;

        totalSent += (size_t)s;
    }

    return true;
}

bool sendError(int sock, const string& message)
{
    uint8_t status = STATUS_ERR;
    uint32_t crc = crc32Init();

    crc = crc32Update(crc, (const char*)&status, 1);
    if (!message.empty())
        crc = crc32Update(crc, message.data(), message.size());

    uint32_t finalCrc = crc32Final(crc);

    if (!sendAll(sock, (const char*)&status, 1))
        return false;

    if (!message.empty() && !sendAll(sock, message.data(), message.size()))
        return false;

    if (!sendAll(sock, (const char*)&finalCrc, sizeof(uint32_t)))
        return false;

    return true;
}

int createListenSocket(int port)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int sock = (int)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        cerr << "[LB ERROR]: Failed to create listen socket.\n";
        exit(1);
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        cerr << "[LB ERROR]: bind() failed on port " << port << ".\n";
        CLOSE_SOCKET(sock);
        exit(1);
    }

    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        cerr << "[LB ERROR]: listen() failed on port " << port << ".\n";
        CLOSE_SOCKET(sock);
        exit(1);
    }

    return sock;
}

int connectToServer(const char* host, int port)
{
    int sock = (int)socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        CLOSE_SOCKET(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        CLOSE_SOCKET(sock);
        return -1;
    }

    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
#ifdef _WIN32
    string cmd = string("start /B server.exe ") + to_string(port);
#else
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
#endif

    int ret = system(cmd.c_str());
    return (ret == 0);
}

int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;

    if (!startServer(serverPort))
        return -1;

    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS; attempt++)
    {
        this_thread::sleep_for(chrono::milliseconds(STARTUP_POLL_INTERVAL_MS));

        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }

    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    return 0;
}