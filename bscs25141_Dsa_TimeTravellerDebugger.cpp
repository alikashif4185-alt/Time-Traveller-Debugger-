// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <iostream>
#include <string>
#include<vector>
#include <cstdint>
#include <fstream>
#include<stdexcept>
#include<sstream>
//#include <unistd.h>
//#include <sys/socket.h
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
        Node(const T& d) {
            data = d;
            next = NULL;
        }
    };
    Node* top;
    Node* prev;
    int32_t count = 0;

public:
    // Implement these functions:
    Stack()
    {
        top = NULL;
        prev = NULL;
        count = 0;
    }
    void push(const T& val)
    {
        Node* temp = new Node(val);
        if (top == NULL) {
            top = temp;
            top->next = NULL;
            count++;
            return;
        }
        temp->next = top;
        top = temp;
        count++;
    }
    T pop() {
        if (top == NULL) {
            return T{};
        }
        if (top->next == NULL) {
            T val = top->data;
            delete top;
            top = NULL;
            count--;
            return val;
        }
        else {
            Node* temp = top;
            T val = top->data;
            top = top->next;
            delete temp;
            temp = NULL;
            count--;
            return val;
        }
    }
    T& peek() {
        if (top == NULL) {
            throw runtime_error("Stack is Empty!");
        }
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth() {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen) {
        int size = 0;
        Node* temp = top;
        if (maxLen < count) {
            return -1;
        }
        while (temp != NULL) {
            out[size] = temp->data;
            temp = temp->next;
            size++;
        }
        return size;
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
    int32_t stepCount=0;

public:
    // Implement these functions
    Timeline() {
        head = tail = NULL;
        stepCount = 0;
    }
    void record(Snapshot* s) {
        TimelineNode* New = new TimelineNode;
        TimelineNode* temp = head;
        New->data = s;
        New->prev = NULL;
        New->next = NULL;
        if (head == NULL) {
            head = tail = New;
            stepCount++;
        }
        else {
            tail->next = New;
            New->prev = tail;
            tail = New;

        }
        stepCount++;
    }
    TimelineNode* begin() {
        return head;
    }
    int32_t getStepCount() {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name=" ";
    int32_t value=0;
};
struct Frame
{
    string func_name="";
    int32_t argc=0;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine=0;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount=0;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth = 0;
};
struct TTDBHeader {
    char magic[4]; // "TTDB"
    int32_t version=0;
    int32_t stepCount=0;
    int64_t indexOffset=0;
};
void writeHeader(FILE* f, const TTDBHeader& h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    fwrite(&h.stepCount, h.stepCount, sizeof(int32_t), f);

    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry {
    string funcName="";
    int64_t byteOffsetInResolveBin=-1; // where this function's FUNC header record sits
};
struct PendingPatch {
    int64_t byteOffsetOfOffsetField=-1; // where in resolve.bin to seek back and overwrite
    string targetFuncName="";
};

// PASS 0x0: READING source.bin + VALIDITY CHECK
vector<string> readSourceLine(ifstream& in)
{
    vector<string>line;
    string l;

    if (!in) {
        cout << "The File Not Found!";
    }
    while (getline(in, l)) {
        line.push_back(l);
    }
    in.close();

    return line;
}
string firstWord(const string& line)
{
    string s;
    int i = 0;
    while (i < line.length() && line[i] != ' ') {
        s += line[i];
        i++;
    }
    return s;

}
string secondWord(const string& line)
{
    string s;int i = 0;
    while (i < line.length() && line[i] != ' ') {
        i++;
    }
    i++;
    while (i < line.length() && line[i] != ' ') {
        s += line[i];
        i++;
    }
    return s;
    
}
bool validateProgram(vector<string> line, const char* sourcePath) {
    Stack<string>st;
     if (line.empty()) {
         cout << "Source file is missing!!";
         return false;
     }
     for (int i = 0;i < line.size();i++) {
         if (((line[i][0] == 's' && line[i][1] == 'e' && line[i][2] == 't') || (line[i][0] == 'a' && line[i][1] == 'd' && line[i][2] == 'd')
             || (line[i][0] == 'm' && line[i][1] == 'u' && line[i][2] == 'l')
             || (line[i][0] == 'd' && line[i][1] == 'i' && line[i][2] == 'v')
             || (line[i][0] == 'c' && line[i][1] == 'a' && line[i][2] == 'l' && line[i][3] == 'l')
             || (line[i][0] == 's' && line[i][1] == 'u' && line[i][2] == 'b')) && st.isEmpty()) {
             return false;
         }
         else if (line[i][0] == 'f' && line[i][1] == 'u' && line[i][2] == 'n' && line[i][3] == 'c' && line[i][4] == ' ' && !st.isEmpty()) {
             return false;
         }
         else if (line[i][0] == 'f' && line[i][1] == 'u' && line[i][2] == 'n' && line[i][3] == 'c' && line[i][4] == ' ' && st.isEmpty()) {
             st.push(line[i]);
         }
         else if (line[i][0] == 'f' && line[i][1] == 'u' && line[i][2] == 'n' && line[i][3] == 'c' &&
             line[i][4] == '_' && line[i][5] == 'e' && line[i][6] == 'n' && line[i][7] == 'd' && st.isEmpty()) {
             return false;
         }
         else if (line[i][0] == 'f' && line[i][1] == 'u' && line[i][2] == 'n' && line[i][3] == 'c' &&
             line[i][4] == '_' && line[i][5] == 'e' && line[i][6] == 'n' && line[i][7] == 'd' && !st.isEmpty()) {
             st.pop();
         }
     }
     return st.isEmpty();
     }
    
// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text) {
    fwrite(&offsetField, 1, 8, f);

    int32_t size = (int32_t)text.size();

    fwrite(&size, 1, 4, f);

    fwrite(text.c_str(), 1, size, f);

    return offsetField + 8 + 4 + size;

    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}

int64_t readResolveRecord(FILE* f, string& outText) {
    int64_t offset;
    int32_t size;
    if(fread(&offset, sizeof(offset), 1, f)!=1) {
        return -1;
    }

    if (fread(&size, sizeof(size), 1, f) != 1) {
        return -1;
    }

    if (size < 0 || size > 1000000) {
        cout << "Out of bound!" << size << endl;
        return -1;
    }

    outText.clear();
    for (int i = 0; i < size; i++) {
        char ch;

        if (fread(&ch, 1, 1, f) != 1) {
            cout << "Not Found!" << endl;
            return -1;
        }

        outText += ch;
    }

    return offset;
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath) {
     FuncEntry func[MAX_FUNCS];
        int32_t f_count = 0;
        PendingPatch patch[MAX_PATCHES];
        int32_t pc = 0;
        ifstream file(sourcePath);
        FILE* f = fopen(resolveBinPath, "wb");
        if (!file.is_open() || f == NULL) {
            if (f != NULL)
                fclose(f);
            file.close();
            return -1;
        }int64_t m_offset = -1;
        string l;
        int64_t offset = 0;
        
        while (getline(file, l)) {
            int64_t adv = writeResolveRecord(f, offset, l);
            if (adv < 0) {
                fclose(f);
                return -1;
            }
            string keyword=firstWord(l);
            string name;

            if (firstWord(l) == "func") {
                name = secondWord(l);
                if (f_count >= MAX_FUNCS) {
                    fclose(f);
                    file.close();
                    return -1;
                }
                func[f_count].funcName = name;
                func[f_count].byteOffsetInResolveBin = offset;
                f_count++;

                if (name == "main")
                    m_offset = offset;
            }
            else if (firstWord(l) == "call") {
                name = secondWord(l);

                if (pc >= MAX_PATCHES) {
                    fclose(f);
                    file.close();
                    return -1;
                }
                patch[pc].targetFuncName = name;
                patch[pc].byteOffsetOfOffsetField = offset;
                pc++;
            }
            offset = adv;
        }
        file.close();
        if (m_offset == -1) {
            fclose(f);
            return -1;
        }
        for (int i = 0; i < pc; i++) {
            bool find = false;
            for (int j = 0; j < f_count; j++) {
                if (patch[i].targetFuncName == func[j].funcName) {
                    int64_t target = func[j].byteOffsetInResolveBin;
                    fseek(f,(long)patch[i].byteOffsetOfOffsetField, SEEK_SET);
                    fwrite(&target, sizeof(target), 1,f);
                    find = true;
                    break;
                }
            }
            if (!find) {
                fclose(f);
                return -1;
            }
        }
        fflush(f);
        fclose(f);
        return m_offset;
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
    int32_t total_tokens=0;
    string text=" ";
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens) {

    string keyword = firstWord(line);
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    string identifier = secondWord(line);
    int32_t i = 0, spcount = 0;
    string s = "";
    while (spcount != 2 && i < line.size()) {
        if (line[i] == ' ') {
            spcount++;
        }
        i++;
    }
    while (i < line.size()) {
        s += line[i];
        i++;
    }
    string param = s;
    tokens[0].text = keyword;
    tokens[0].type = KEYWORD;
    tokens[1].text = identifier;
    tokens[1].type = IDENTIFIER;
    tokens[2].text = param;
    tokens[2].type = PARAM;

    return 3;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack,Timeline* timeline) {
    Snapshot* newSnap = new Snapshot();
    int size = callStack.depth();
    int tsize = callStack.snapshot_into(newSnap->callStack, newSnap->stackDepth);
    if (size >= 0) {
        newSnap->stackDepth = size;
   }
    else {
        newSnap->stackDepth = 0;
    }
        timeline->record(newSnap);

    return newSnap;
}

void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline* timeline) {
    Stack<Frame> callStack;
    Frame main;
    callStack.push(main);

    FILE* file = fopen(resolveBinPath, "rb");

    if (file == NULL) {
        cout << "Error!  does not open file resolve.bin" << endl;
        return;
    }

    string l;
    int64_t offset;
    int k = 0;
    Token token[MAX_TOKENS];

    cout << " mainOffset = " << mainOffset << endl;
    cout << "Debug size = " << sizeof(long) << endl;

    if (fseek(file, (long)mainOffset, SEEK_SET) != 0) {
        perror("fseek failed");
        fclose(file);
        return;
    }

    while (true) {
        offset = readResolveRecord(file, l);
        if (offset == -1) {
            cout << "File end!" << endl;
            break;
        }

        cout << "Offset: " << offset << endl;
        cout << "Line: (" << l << ")" << endl;

        int tokenize = tokenizeLine(l, token, 10);

        cout << "Tokens: " << tokenize << endl;

        if (tokenize <= 0) {
            continue;
        }
        if (token[0].text == "add") {
            k += stoi(token[2].text);
        }
        else if (token[0].text == "sub") {
            k -= stoi(token[2].text);
        }
        else if (token[0].text == "mul") {
            k *= stoi(token[2].text);
        }
        else if (token[0].text == "div") {
            k /= stoi(token[2].text);
        }
        Frame* curr = &callStack.peek();
        if (token[0].text == "set") {
            if (curr->localCount < MAX_VARS_PER_FRAME) {
                curr->locals[curr->localCount].name = token[1].text;
                curr->locals[curr->localCount].value = stoi(token[2].text);
                curr->localCount++;
            }
        }
        if (token[0].text == "add") {
            for (int i = 0;i < curr->localCount;i++) {

                if (curr->locals[i].name == token[1].text) {
                    curr->locals[i].value += stoi(token[2].text);
                }
            }
        }
        if (token[0].text == "sub") {
            for (int i = 0;i < curr->localCount;i++) {
                if (curr->locals[i].name == token[1].text) {
                    curr->locals[i].value -= stoi(token[2].text);
                }
            }
        }
        if (token[0].text == "mul") {
            for (int i = 0;i < curr->localCount;i++) {

                if (curr->locals[i].name == token[1].text) {
                    curr->locals[i].value *= stoi(token[2].text);
                }
            }
        }
        if (token[0].text == "div") {
            for (int i = 0;i < curr->localCount;i++) {

                if (curr->locals[i].name == token[1].text) {
                    curr->locals[i].value /= stoi(token[2].text);
                }
            }
        }
        if (token[0].text == "func_end") {
            cout << "operation end" << endl;
            break;
        }
       
        for (int i = 0; i < curr->localCount; i++) {
            cout << "Variable: " << curr->locals[i].name
                << " = " << curr->locals[i].value << endl;
        }
        cout << "Count: " << curr->localCount << endl;

        for (int i = 0; i < curr->localCount; i++) {
            cout << curr->locals[i].name << " = "
                << curr->locals[i].value << endl;
        }

        Snapshot* s = buildSnapshot(callStack, timeline);
       
    }



    fclose(file);
}


// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline* timeline, const char* tdbgPath) {
    ofstream file(tdbgPath,ios::binary);
    int total = timeline->getStepCount();
    vector<int>arr={};
    int count = 0;
    TimelineNode* time;
    time = timeline->begin();
    int curroff = 20;
    while (time != NULL && count<total) {

        arr.push_back(curroff);
        curroff += sizeof(Snapshot);
        time = time->next;
    }
    time = timeline->begin();
    int i = 0;
    while (time != NULL && i <arr.size()) {
        file.write((char*)&time->data, sizeof(Snapshot));
        time = time->next;
        i++;
    }
    for (int i = 0;i <arr.size();i++) {
        file.write((char*)&arr[i], sizeof(arr[i]));
    }
    file.close();
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
    
    // PASS 0x3: SERIALIZE TIMELINE
    


// main section
int32_t main()
{
   
        ifstream in("source.bin");
        if (!in.is_open()) {
            cout << "[ERROR] Could not open source.bin. Make sure it is in your project directory!" << endl;
            return 1;
        }

        vector<string> l = readSourceLine(in);
      


        if (!validateProgram(l,"source.bin"))
        {
            // send an error response instead of a .tdbg file
            return 1;
        }

        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

        Timeline *timeline=new Timeline();
        executeProgram("resolve.bin", mainOffset, timeline);

        writeTdbg(timeline, "session.tdbg");

        delete timeline;
        return 0;
    }


