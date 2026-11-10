#ifndef HELPER_H
#define HELPER_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

typedef intptr_t ssize_t;
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif

#define CLOSE_SOCKET(s) closesocket(s)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define CLOSE_SOCKET(s) close(s)
#endif

#include <iostream>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

using namespace std;


// Server Constants
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64 * 1024;


// Server & LB & Client Structs
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};

struct Token
{
    enum TokenType type;
    string text;
};

struct Variable
{
    string name;
    int32_t value;
};

struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct ClientInfo
{
    int sock;
    string address;
    int port;
    time_t arrivedAt;
};



// Streaming CRC-32 & TrailerChecker
const uint8_t STATUS_OK = 0;
const uint8_t STATUS_ERR = 1;

inline uint32_t crc32Update(uint32_t crc, const char* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
    }
    return crc;
}

inline uint32_t crc32Init()
{
    return 0xFFFFFFFFU;
}

inline uint32_t crc32Final(uint32_t crc)
{
    return ~crc;
}

struct TrailerChecker
{
    uint32_t crcState;
    uint8_t tail[4];
    size_t tailLen;
    uint64_t totalPopped;

    TrailerChecker()
    {
        reset();
    }

    void reset()
    {
        crcState = crc32Init();
        tailLen = 0;
        totalPopped = 0;
    }

    void feed(const char* data, size_t len, string& outBytes)
    {
        outBytes.clear();
        if (len == 0)
            return;

        for (size_t i = 0; i < len; i++)
        {
            uint8_t b = (uint8_t)data[i];
            if (tailLen < 4)
            {
                tail[tailLen++] = b;
            }
            else
            {
                char confirmed = (char)tail[0];
                crcState = crc32Update(crcState, &confirmed, 1);
                outBytes.push_back(confirmed);
                totalPopped++;

                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = tail[3];
                tail[3] = b;
            }
        }
    }

    bool verify() const
    {
        if (tailLen < 4 || totalPopped < 1)
            return false;

        uint32_t expectedCrc = 0;
        memcpy(&expectedCrc, tail, sizeof(uint32_t));

        uint32_t actualCrc = crc32Final(crcState);
        return (expectedCrc == actualCrc);
    }

    const char* getTrailerBytes() const
    {
        return (const char*)tail;
    }
};

#endif // HELPER_H