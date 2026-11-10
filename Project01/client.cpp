// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <vector>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE* file;
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================
bool recvAll(int sock, char* buf, size_t n)
{
    size_t totalReceived = 0;
    while (totalReceived < n)
    {
        ssize_t r = recv(sock, buf + totalReceived, n - totalReceived, 0);
        if (r <= 0)
            return false;

        totalReceived += (size_t)r;
    }

    return true;
}

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

int connectToHost(const char* host, int port)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
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

bool uploadSource(int sock, const char* sourcePath)
{
    FILE* f = fopen(sourcePath, "rb");
    if (!f)
    {
        cout << "Could not open source file: " << sourcePath << "\n";
        return false;
    }

    uint32_t crc = crc32Init();

    char statusByte = (char)STATUS_OK;
    crc = crc32Update(crc, &statusByte, 1);
    if (!sendAll(sock, &statusByte, 1))
    {
        fclose(f);
        return false;
    }

    vector<char> buf(IO_BUFFER_SIZE);
    size_t bytesRead = 0;
    while ((bytesRead = fread(buf.data(), 1, buf.size(), f)) > 0)
    {
        crc = crc32Update(crc, buf.data(), bytesRead);

        if (!sendAll(sock, buf.data(), bytesRead))
        {
            fclose(f);
            return false;
        }
    }

    fclose(f);

    uint32_t finalCrc = crc32Final(crc);
    if (!sendAll(sock, (const char*)&finalCrc, sizeof(uint32_t)))
        return false;

    shutdown(sock, SHUT_WR);
    return true;
}

bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    outError.clear();
    TrailerChecker checker;
    vector<char> buf(IO_BUFFER_SIZE);
    string confirmedBytes;

    bool statusRead = false;
    uint8_t status = STATUS_OK;
    FILE* outFile = nullptr;

    while (true)
    {
        ssize_t r = recv(sock, buf.data(), buf.size(), 0);
        if (r < 0)
        {
            if (outFile)
            {
                fclose(outFile);
                remove(outTdbgPath);
            }

            outError = "Connection error while receiving response";
            return false;
        }
        if (r == 0)
            break;

        checker.feed(buf.data(), (size_t)r, confirmedBytes);
        if (confirmedBytes.empty())
            continue;

        size_t offset = 0;
        if (!statusRead)
        {
            status = (uint8_t)confirmedBytes[0];
            statusRead = true;
            offset = 1;

            if (status == STATUS_OK)
            {
                outFile = fopen(outTdbgPath, "wb");

                if (!outFile)
                {
                    outError = "Could not create local .tdbg file";
                    return false;
                }
            }
        }

        size_t remaining = confirmedBytes.size() - offset;
        if (remaining > 0)
        {
            if (status == STATUS_OK) fwrite(confirmedBytes.data() + offset, 1, remaining, outFile);
            else outError.append(confirmedBytes.data() + offset, remaining);
        }
    }

    if (outFile)
    {
        fclose(outFile);
        outFile = nullptr;
    }

    if (!statusRead || !checker.verify())
    {
        if (status == STATUS_OK)
            remove(outTdbgPath);

        outError = "response corrupted";

        return false;
    }

    if (status != STATUS_OK)
    {
        if (outError.empty())
            outError = "Unknown error from server";

        return false;
    }

    return true;
}

bool requestDebugFile(const char* host, int port, const char* sourcePath, const char* outTdbgPath, string& outError)
{
    // Verify the local source file exists before opening a socket
    FILE* testFile = fopen(sourcePath, "rb");
    if (!testFile)
    {
        outError = string("Could not open file: ") + sourcePath;
        return false;
    }
    fclose(testFile);

    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "Could not connect to load balancer";
        return false;
    }

    // Apply 30-second receive timeout so client doesn't hang forever
#ifdef _WIN32
    DWORD timeoutMs = 30000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
#else
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (!uploadSource(sock, sourcePath))
    {
        CLOSE_SOCKET(sock);
        outError = "Failed to upload source file";
        return false;
    }

    bool ok = receiveResponse(sock, outTdbgPath, outError);
    CLOSE_SOCKET(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================
bool readStringFromTdbg(FILE* f, string& out)
{
    int32_t len = 0;
    if (fread(&len, sizeof(int32_t), 1, f) != 1)
        return false;

    if (len < 0 || len > 65536)
        return false;

    out.resize(len);
    if (len > 0)
    {
        if (fread(&out[0], 1, len, f) != (size_t)len)
            return false;
    }
    return true;
}

bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    if (!r.file)
        return false;

    if (fread(r.header.magic, 1, 4, r.file) != 4 ||
        fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (memcmp(r.header.magic, "TTDB", 4) != 0 || r.header.version != 1 || r.header.stepCount <= 0 || r.header.indexOffset < 20)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    if (fseek(r.file, (long)r.header.indexOffset, SEEK_SET) != 0)
    {
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    r.index.resize(r.header.stepCount);
    if (fread(r.index.data(), sizeof(int64_t), r.header.stepCount, r.file) != (size_t)r.header.stepCount)
    {
        r.index.clear();
        fclose(r.file);
        r.file = nullptr;
        return false;
    }

    return true;
}

void closeTdbg(TdbgReader& r)
{
    r.index.clear();

    if (r.file)
    {
        fclose(r.file);
        r.file = nullptr;
    }
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (!r.file || step < 0 || step >= r.header.stepCount)
        return nullptr;

    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0)
        return nullptr;

    Snapshot* s = new Snapshot();

    if (fread(&s->stackDepth, sizeof(int32_t), 1, r.file) != 1 ||
        s->stackDepth < 0 || s->stackDepth > MAX_STACK_DEPTH)
    {
        delete s;
        return nullptr;
    }

    for (int32_t i = 0; i < s->stackDepth; i++)
    {
        Frame& fr = s->callStack[i];

        if (!readStringFromTdbg(r.file, fr.func_name))
        {
            delete s;
            return nullptr;
        }

        if (fread(&fr.argc, sizeof(int32_t), 1, r.file) != 1 ||
            fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        {
            delete s;
            return nullptr;
        }

        for (int32_t j = 0; j < fr.argc; j++)
        {
            if (!readStringFromTdbg(r.file, fr.argv[j].name) ||
                fread(&fr.argv[j].value, sizeof(int32_t), 1, r.file) != 1)
            {
                delete s;
                return nullptr;
            }
        }

        if (fread(&fr.returnLine, sizeof(int32_t), 1, r.file) != 1)
        {
            delete s;
            return nullptr;
        }

        if (fread(&fr.localCount, sizeof(int32_t), 1, r.file) != 1 ||
            fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        {
            delete s;
            return nullptr;
        }

        for (int32_t j = 0; j < fr.localCount; j++)
        {
            if (!readStringFromTdbg(r.file, fr.locals[j].name) ||
                fread(&fr.locals[j].value, sizeof(int32_t), 1, r.file) != 1)
            {
                delete s;
                return nullptr;
            }
        }
    }

    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.currentStep = 0;
    s.reader.file = nullptr;
    s.reader.file_index = -1;
    s.reader.header.stepCount = 0;
    s.reader.header.indexOffset = 0;
    s.reader.index.clear();
}

void closeSession(DebugSession &s)
{
    if (s.active_flag)
    {
        closeTdbg(s.reader);
    }

    s.active_flag = 0;
    s.currentStep = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s);

bool showCurrent(DebugSession& s)
{
    Snapshot* snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (!snap)
    {
        cout << "[ERROR] damaged .tdbg file.\n";
        return false;
    }

    printSnapshot((int32_t)s.currentStep, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}

bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "No active debugging session. Run 'start <file>' first.\n";
        return false;
    }

    int64_t targetStep = s.currentStep + (int64_t)delta;

    if (targetStep < 0)
    {
        cout << "Cannot move backward: out of bounds (currently at step "
            << (s.currentStep + 1) << " / " << s.reader.header.stepCount << ").\n";

        return false;
    }

    if (targetStep >= s.reader.header.stepCount)
    {
        cout << "Cannot move forward: out of bounds (currently at step "
            << (s.currentStep + 1) << " / " << s.reader.header.stepCount << ").\n";

        return false;
    }

    s.currentStep = targetStep;
    return showCurrent(s);
}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    const char* tempPath = "ttdb_downloading.tdbg";
    string errorMsg;

    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), tempPath, errorMsg))
    {
        cout << errorMsg << "\n";
        return false;
    }

    TdbgReader tempReader;
    tempReader.file = nullptr;
    tempReader.file_index = -1;
    if (!openTdbg(tempPath, tempReader))
    {
        remove(tempPath);
        cout << "server sent a damaged .tdbg file\n";

        return false;
    }
    closeTdbg(tempReader);

    if (s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }

    remove(TDBG_FILE);
    rename(tempPath, TDBG_FILE);

    TdbgReader newReader;
    newReader.file = nullptr;
    newReader.file_index = -1;
    if (!openTdbg(TDBG_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file\n";
        return false;
    }

    s.reader = newReader;
    s.currentStep = 0;
    s.active_flag = 1;

    return showCurrent(s);
}

bool cmdNext(DebugSession& s)
{
    return moveBy(s, 1);
}

bool cmdPrv(DebugSession& s)
{
    return moveBy(s, -1);
}

bool cmdJmpNext(DebugSession& s, int32_t n)
{
    if (n <= 0 || n > MAX_JUMP)
    {
        cout << "[ERROR] Invalid jump distance. Must be between 1 and " << MAX_JUMP << ".\n";
        return false;
    }

    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession& s, int32_t n)
{
    if (n <= 0 || n > MAX_JUMP)
    {
        cout << "[ERROR] Invalid jump distance. Must be between 1 and " << MAX_JUMP << ".\n";
        return false;
    }

    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "==================== Step " << (step + 1) << " / " << totalSteps << " (Stack Depth: " << s.stackDepth << ") ====================\n";

    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame& fr = s.callStack[i];
        cout << "  [" << i << "] func " << fr.func_name << " (args: ";

        if (fr.argc == 0)
        {
            cout << "none";
        }
        else
        {
            for (int32_t j = 0; j < fr.argc; j++)
            {
                if (j > 0) cout << ", ";
                cout << fr.argv[j].name << "=" << fr.argv[j].value;
            }
        }
        cout << " | locals: ";


        if (fr.localCount == 0)
        {
            cout << "none";
        }
        else
        {
            for (int32_t j = 0; j < fr.localCount; j++)
            {
                if (j > 0) cout << ", ";
                cout << fr.locals[j].name << "=" << fr.locals[j].value;
            }
        }
        cout << ")\n";
    }

    cout << "====================================================================\n";
}

void printHelp()
{
    cout << "Available commands:\n"
        << " - start <file> - Upload source file and start debugging session\n"
        << " - next         - Move forward 1 step\n"
        << " - prv          - Move backward 1 step\n"
        << " - jmp next <n> - Jump forward <n> steps\n"
        << " - jmp prv <n>  - Jump backward <n> steps\n"
        << " - help         - Show this help message\n"
        << " - quit         - Close session and exit\n";
}


// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================
bool parsePositiveInt(const string& str, int32_t& outVal)
{
    if (str.empty())
        return false;

    for (size_t i = 0; i < str.size(); i++)
    {
        if (!isdigit((unsigned char)str[i]))
            return false;
    }

    try
    {
        long long val = stoll(str);
        if (val <= 0 || val > MAX_JUMP)
            return false;

        outVal = (int32_t)val;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    vector<string> words;
    size_t i = 0;

    while (i < line.size())
    {
        while (i < line.size() && isspace((unsigned char)line[i]))
            i++;

        if (i >= line.size())
            break;

        size_t start = i;
        while (i < line.size() && !isspace((unsigned char)line[i]))
            i++;

        words.push_back(line.substr(start, i - start));
    }

    if (words.empty())
        return true;

    if (words[0] == PROGRAM_NAME)
    {
        words.erase(words.begin());

        if (words.empty())
            return true;
    }


    const string& cmd = words[0];

    if (cmd == "start" && words.size() == 2)
        return cmdStart(s, words[1]);

    else if (cmd == "next" && words.size() == 1)
        return cmdNext(s);

    else if (cmd == "prv" && words.size() == 1)
        return cmdPrv(s);

    else if (cmd == "jmp" && words.size() == 3)
    {
        int32_t n = 0;
        if (parsePositiveInt(words[2], n))
        {
            if (words[1] == "next")
                return cmdJmpNext(s, n);

            if (words[1] == "prv")
                return cmdJmpPrv(s, n);
        }
    }
    else if (cmd == "help" && words.size() == 1)
    {
        printHelp();
        return true;
    }
    else if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
    {
        quitRequested = true;
        return true;
    }

    cout << "unknown command\n";
    printHelp();

    return false;
}

int main(int argc, char* argv[])
{
    if (argc >= 3)
    {
        LB_HOST = argv[1];
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "=== Time-Travel Debugger Client (" << PROGRAM_NAME << ") ===\n";
    cout << "Connected target LB: " << LB_HOST << ":" << LB_PORT << "\n";
    printHelp();

    bool quitRequested = false;
    string line;

    while (!quitRequested)
    {
        cout << PROMPT;
        cout.flush();

        if (!getline(cin, line))
            break;

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;
}