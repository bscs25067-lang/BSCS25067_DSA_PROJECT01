// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
// #include <unistd.h>
// #include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    {
        top = nullptr;
        count = 0; 
    }
    void push(const T& val)
    {
        if (count == MAX_STACK_DEPTH) return;

        Node* newNode = new Node();
        newNode->data = val;
        newNode->next = top;

        top = newNode;
        count++;
    }
    T pop()
    {
        if (isEmpty())
            throw runtime_error("Stack underflow");

        Node* nextNode = top->next;
        T data = top->data;

        delete top;
        top = nextNode;

        count--;

        return data;
    }
    T& peek()
    {
        if (isEmpty())
            throw runtime_error("Stack underflow");

        return top->data;
    }
    bool isEmpty() { return count == 0; }
    int32_t depth() { return count; }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* currentNode = top;
        int32_t count = 0;

        while (currentNode != nullptr && count < maxLen)
        {
            out[count++] = currentNode->data;
            currentNode = currentNode->next;
        }

        return count;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        TimelineNode* newNode = new TimelineNode();
        newNode->data = s;
        newNode->next = nullptr;

        if (head == nullptr)
        {
            newNode->prev = nullptr;
            head = tail = newNode;
        }
        else
        {
            newNode->prev = tail;
            tail->next = newNode;
            tail = newNode;
        }

        stepCount++;
    }
    TimelineNode* begin() { return head; }
    int32_t getStepCount() { return stepCount; }
};

// Core structs
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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    while (getline(in, out))
    {
        if (!out.empty() && out.back() == '\r')
            out.pop_back();

        bool isBlank = true;
        for (char c : out)
        {
            if (!isspace(c))
            {
                isBlank = false;
                break;
            }
        }

        if (!isBlank) return true;
    }

    return false;
}
string firstWord(const string& line)
{
    size_t start = 0;
    while (start < line.length() && isspace(line[start]))
        start++;

    size_t end = start;
    while (end < line.length() && !isspace(line[end]))
        end++;

    size_t wordLen = end - start;
    return line.substr(start, wordLen);
}
string secondWord(const string& line)
{
    size_t i = 0;

    while (i < line.length() && isspace(line[i]))
        i++;

    while (i < line.length() && !isspace(line[i]))
        i++;

    while (i < line.length() && isspace(line[i]))
        i++;

    size_t start = i;
    while (i < line.length() && !isspace(line[i]))
        i++;

    size_t wordLen = i - start;
    return line.substr(start, wordLen);
}
bool validateProgram(const char* sourcePath)
{
    ifstream file(sourcePath);
    if (!file.is_open())
    {
        cerr << "[ERROR]: Could not open file \"" << sourcePath << "\".";
        return false;
    }

    string line;
    Stack<string> scopeStack;
    int32_t lineNumber = 0;
    
    while (readSourceLine(file, line))
    {
        lineNumber++;
        string w = firstWord(line);

        if (w == "func")
        {
            if (!scopeStack.isEmpty())
            {
                cerr << "[ERROR]: Validation Error at line " << lineNumber << ": Nested 'func' declarations are not allowed.\n";
                return false;
            }

            scopeStack.push(secondWord(line));
        }
        else if (w == "func_end")
        {
            if (scopeStack.isEmpty())
            {
                cerr << "[ERROR]: Validation Error at line " << lineNumber << ": Found 'func_end' without a matching 'func'.\n";
                return false;
            }

            scopeStack.pop();
        }
    }

    if (!scopeStack.isEmpty())
    {
        cerr << "[ERROR]: Validation Error Missing'func_end'. Reached the end of file while still inside a function.\n";
        return false;
    }

    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t startPos = ftell(f);

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    
    int32_t size = text.length();
    fwrite(&size, sizeof(int32_t), 1, f);

    fwrite(text.c_str(), 1, size, f);

    return startPos;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField;
    fread(&offsetField, sizeof(int64_t), 1, f);

    int32_t size;
    fread(&size, sizeof(int32_t), 1, f);

    outText.resize(size);
    fread(&outText[0], 1, size, f);

    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream inFile(sourcePath);
    FILE* f = fopen(resolveBinPath, "wb+");
    if (!inFile || !f)
    {
        cerr << "[ERROR]: Could not open file for resolving.\n";
        if (f) fclose(f);
        return -1;
    }

    // Copy all lines & Store Functions
    string line;
    while (readSourceLine(inFile, line))
    {
        string fw = firstWord(line);
        int64_t pos = writeResolveRecord(f, 0, line);

        if (fw == "func")
        {
            if (funcCount >= MAX_FUNCS)
            {
                cerr << "[ERROR]: Too many functions.\n";
                fclose(f);
                return -1;
            }

            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (fw == "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                cerr << "[ERROR]: Too many calls\n"; fclose(f);
                return -1;
            }

            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
    }

    // Get Main
    int64_t mainFuncOffset = -1;
    for (int i = 0; i < funcCount; i++)
    {
        if (funcArray[i].funcName == "main")
            mainFuncOffset = funcArray[i].byteOffsetInResolveBin;
    }

    if (mainFuncOffset == -1)
    {
        cerr << "[ERROR]: No main function.\n";
        fclose(f);

        return -1;
    }
    
    // Patch Calls
    for (int i = 0; i < patchCount; i++)
    {
        int64_t target = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
                target = funcArray[j].byteOffsetInResolveBin;
        }

        if (target == -1)
        {
            cerr << "[Error]: Call to undefined function " << patches[i].targetFuncName << ".\n";
            fclose(f);
            return -1;
        }

        fseek(f, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&target, sizeof(int64_t), 1, f);
    }

    fclose(f);
    return mainFuncOffset;
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t count = 0;
    size_t i = 0;
    while (i < line.size() && count < maxTokens)
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            i++;

        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t')
            i++;

        size_t wordSize = i - start;
        tokens[count].text = line.substr(start, wordSize);

        if (count == 0) tokens[count].type = KEYWORD;
        else if(count == 1) tokens[count].type = IDENTIFIER;
        else tokens[count].type = PARAM;

        count++;
    }

    return count;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* s = new Snapshot();
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    
    return s;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}