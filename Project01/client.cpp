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

// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

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

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
}

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
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

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

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
    return false;
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

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

}
