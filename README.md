# Time-Travel Debugger (Phase 01 - Server)
 
The server takes a program written in C-- (`source.bin`), runs it, and saves the state of the call stack after every line into a `session.tdbg` file. The client (Phase 02) will use this file to move backward and forward through the execution.
 
## How to run
 
Put a `source.bin` file in the same folder as the program.
 
It creates `resolve.bin` and `session.tdbg`. If something is wrong with the program, it prints an error and stops without creating the `.tdbg` file.
 
## Language (C--)
 
Instructions: `func`, `func_end`, `call`, `set`, `add`, `sub`, `mul`, `div`
 
- `add`, `sub`, `mul`, `div` are Arithmetic functions
- `func`, `func_end` are used for defining function blocks
- `call` is used for calling functions
- `set` is used for setting and assigning values to variables
- `main` is the function block that is always executed

## Data structures
 
`Stack` and `Timeline` are written using linked lists. The only built-in container used is `std::vector`.