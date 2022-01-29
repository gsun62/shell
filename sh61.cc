#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
using namespace std;

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__


// struct redir
//    Data structure describing a single redirection.

struct redir {
    int redirect_op = -1; // by default
    string filename; // follows the operator
};


// struct command
//    Data structure describing a command.

struct command {
    vector<string> args; // stores all arguments of the command
    pid_t pid = -1; // process ID running this command, -1 if none
    pid_t run();

    command();
    ~command();
    
    command* next = nullptr; // link to the next command (linked-list implementation)

    int op = TYPE_NORMAL; // operator following the command
    int redirect_op = -1; // detects redirections: 0 = in, 1 = out, 2 = err
    int exit_status = 1; // default = failure
    
    vector<redir*> redirections; // stores redirections for a command
};


// command::command()
//    This constructor function initializes a `command` structure. You may
//    add stuff to it as you grow the command structure.

command::command() {
}


// command::~command()
//    This destructor function is called to delete a command.

command::~command() {

    // if there exists a next command, delete it
    if (next) {
        delete next;
    }

    // delete all elements of the redirections vector
    for (int i = 0; i < (int) redirections.size(); i++) {
        delete redirections[i];
    }
}


// COMMAND EXECUTION

pid_t pgid; // global to track process group IDs

void run_list(command* c);


// command::run()
//    Create a single child process running the command in `this`.
//    Sets `this->pid` to the pid of the child process and returns `this->pid`.

pid_t command::run() {
    // create a pipe if necessary
    int pfd[2];
    if (this->op == TYPE_PIPE) {
        pipe(pfd);
    }
    
    pid_t f_pid = fork(); // we create a new process

    if (f_pid == 0) { // in the child

        // pipe dance
        if (this->op == TYPE_PIPE) {
            dup2(pfd[1], 1);
            close(pfd[1]);
            close(pfd[0]);
        }
        
        // our command has a vector of arguments, 
         // so we convert those arguments to const char* so we can use them later here
        vector<const char*> arr;
        for (size_t i = 0; i < this->args.size(); i++) { 
            arr.push_back(this->args[i].c_str());
        }
        arr.push_back(nullptr);
        
        // handle commands with redirections
        redir* r; // makes our code more readable
        for (int i = 0; i < (int) this->redirections.size(); i++) {
            r = this->redirections[i];
            int res = -1; // default value
            if (r->redirect_op == 0) { // stdin
                res = open(r->filename.c_str(), O_RDONLY);
            } else if (r->redirect_op == 1 || r->redirect_op == 2) { // stdout or stderr
                res = open(r->filename.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
            }
            if (res == -1) { // redirection is unsuccessful
                fprintf(stderr, "No such file or directory\n");
                _exit(1);
            }

            // set up the redirections after opening
            dup2(res, r->redirect_op);
            close(res);
        }

        // now we actually execute the command
        if (execvp(arr[0], (char* const*) arr.data()) == 0) {
            _exit(0);
        } else {
            fprintf(stderr, "ERROR: execvp failed to execute");
            _exit(1);
        }
    } else { // in the parent
        if (this->op == TYPE_PIPE) {
            // pipe dance
            close(pfd[1]);
            dup2(pfd[0], 0);
            close(pfd[0]);
        }
        this->pid = f_pid;
        setpgid(f_pid, pgid); // handle race conditions
    } 
    return this->pid;
}


// run_pipeline(c)
//    Run a pipeline starting at c, defined as commands joined by |. The only structure
//    at a lower level than this are single commands.

void run_pipeline(command* c){
    // set up process groups
    pgid = getpid();
    setpgid(getpid(), pgid);
    
    // loop to run all the commands within this pipeline
    while (c->op == TYPE_PIPE) {
        c->run();
        c = c->next;
    }
    c->run(); // we still need to run the last command, which isn't TYPE_PIPE
    
    claim_foreground(pgid); // attaches terminal to the pgid process group
    assert (waitpid(c->pid, &c->exit_status, 0) > 0); // set the status of the last command
    claim_foreground(0); // disconnects terminal from the process group
}


// run_conditional(c)
//    Run a conditional starting at c, defined as pipelines joined by && or ||.

void run_conditional(command* c){
   
    // loop to check for cd
    command* chead = c;
    while (true) { 
        if (c->args[0] == "cd") {
            chdir(c->args[1].c_str());
        }
        if (c->op == TYPE_BACKGROUND || c->op == TYPE_SEQUENCE || c->next == nullptr) {
            break; // break at the end of the conditional
        }
        c = c->next;
    }
    c = chead;
    
    // fork to run the conditional, as this allows us to run in parallel when needed
    pid_t f_pid = fork();

    if (f_pid == 0) { // in the child
        setpgid(0, 0);
        command* pipeline_head = c; // we save this for when we run pipelines within the conditional

        // loop to run all the pipelines within this conditional
        while (c) {

            // marks the end of the current pipeline
            while (c->op == TYPE_PIPE) {
                c = c->next;
            }
            run_pipeline(pipeline_head); // runs the current pipeline from its head

            if (c->op == TYPE_BACKGROUND || c->op == TYPE_SEQUENCE) {
                break; // break because we've reached the end of the conditional
            } else if (c->op == TYPE_AND) {
                // case where the subsequent commands should not run
                if (WIFEXITED(c->exit_status) == 0 || WEXITSTATUS(c->exit_status) != 0) {
                    while (c->op==TYPE_AND){ // all other ands after the first fail should fail
                        c = c->next; // thus, we skip them
                    }
                }
            } else if (c->op == TYPE_OR) {
                // case where the subsequent commands should not run
                if (WIFEXITED(c->exit_status) && WEXITSTATUS(c->exit_status) == 0) {
                    while (c->op==TYPE_OR){ // all other ors after the first fail should fail
                        c = c->next;
                    }
                }                
            }
            c = c->next; 
            pipeline_head = c;
        }
        _exit(0); // exit our forked process
    } else { // in the parent
        c->pid = f_pid;
    }
}


// run_list(c)
//    Run the command *list* starting at `c`.

void run_list(command* c) {
    setpgid(0, 0);
    command* conditional_head = c; // we save this for when we run conditionals within the list

    // loop to run all the conditionals within this list
    while (c) {

        // mark the end of the current conditional 
        while (c->op != TYPE_SEQUENCE && c->op != TYPE_BACKGROUND && c->next) {
            c = c->next; // iterate through the chain until you reach a list operator
        }
        run_conditional(conditional_head); // runs the current conditional from the head of the chain

        // background processes run in parallel, so we don't wait for them
        if (c->op != TYPE_BACKGROUND) {
            assert (waitpid(conditional_head->pid, &c->exit_status, 0) > 0);
        }
        c = c->next;
        conditional_head = c;
    }
}


// parse_line(s)
//    Parse the command list in `s` and return it. Returns `nullptr` if
//    `s` is empty (only spaces). You’ll extend it to handle more token
//    types.

command* parse_line(const char* s){
    shell_parser parser(s);

    command* chead = new command; // stores the first command in the list
    command* c = chead; // current command being built
    command* prev = c; // previous command built
    
    int redirect_op = -1; // default for when there aren't redirections
    redir* r = nullptr;
    
    for (shell_token_iterator it = parser.begin(); it != parser.end(); ++it) {
        if (it.type() == TYPE_REDIRECT_OP) {

            // we keep track of the specific redirect operator here
            // recall that 0 is for input, 1 is for output, and 2 for error)
            if (it.str() == "<") {
                redirect_op = 0;
            } else if (it.str() == ">") {
                redirect_op = 1;
            } else if(it.str() == "2>") {
                redirect_op = 2;
            } 
        } else if (it.type() != TYPE_NORMAL) { // handling operators 
            c->op = it.type();
            prev = c;
            c->next = new command; // we move on, since operators mark the end of a command
            c = c->next;
        } else if (redirect_op == -1) {
            c->args.push_back(it.str()); // we add to the arguments vector if no redirections are involved
        } else { // handling commands with redirections
            r = new redir;
            r->redirect_op = redirect_op;
            redirect_op = -1;
            r->filename = it.str();

            // we initialized a redir struct with the information from the string 
            // and now we add it to the redirections vector for this command
            c->redirections.push_back(r); 
        }
    }

    // At the end of each iteration of the above loop, we create a new command for c->next and 
    // then set that equal to the current command. Here, we are deleting the extra, unneccesary command
    // created at the end of the command list.
    if (prev->next && prev->next->args.size() == 0) {
        delete prev->next;
        prev->next = nullptr;
    }
    return chead;
}


int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (command* c = parse_line(buf)) {
                run_list(c);
                delete c;
            }
            bufpos = 0;
            needprompt = 1;
        }

        // handle zombie processes and/or interrupt requests
        while (true) {
            if (waitpid(-1, nullptr, WNOHANG) <= 0) { // we wait for any child process, nonblocking
                break; // we exit the loop once we reach a process that has been terminated by an exit
            }
        }
    }
    return 0;
}