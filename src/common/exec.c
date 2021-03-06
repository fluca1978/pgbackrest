/***********************************************************************************************************************************
Execute Process
***********************************************************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/exec.h"
#include "common/io/handleRead.h"
#include "common/io/handleWrite.h"
#include "common/io/io.h"
#include "common/io/read.intern.h"
#include "common/io/write.intern.h"
#include "common/wait.h"

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct Exec
{
    MemContext *memContext;                                         // Mem context
    String *command;                                                // Command to execute
    StringList *param;                                              // List of parameters to pass to command
    const String *name;                                             // Name to display in log/error messages
    TimeMSec timeout;                                               // Timeout for any i/o operation (read, write, etc.)

    pid_t processId;                                                // Process id of the child process

    int handleRead;                                                 // Read handle
    int handleWrite;                                                // Write handle
    int handleError;                                                // Error handle

    IoHandleRead *ioReadHandle;                                     // Handle read driver
    IoWrite *ioWriteHandle;                                         // Handle write interface

    IoRead *ioReadExec;                                             // Wrapper for handle read interface
    IoWrite *ioWriteExec;                                           // Wrapper for handle write interface
};

/***********************************************************************************************************************************
New object
***********************************************************************************************************************************/
Exec *
execNew(const String *command, const StringList *param, const String *name, TimeMSec timeout)
{
    FUNCTION_LOG_BEGIN(logLevelDebug)
        FUNCTION_LOG_PARAM(STRING, command);
        FUNCTION_LOG_PARAM(STRING_LIST, param);
        FUNCTION_LOG_PARAM(STRING, name);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
    FUNCTION_LOG_END();

    ASSERT(command != NULL);
    ASSERT(name != NULL);
    ASSERT(timeout > 0);

    Exec *this = NULL;

    MEM_CONTEXT_NEW_BEGIN("Exec")
    {
        this = memNew(sizeof(Exec));
        this->memContext = MEM_CONTEXT_NEW();

        this->command = strDup(command);

        // Parameter list is optional but if not specified we need to build one with the command
        if (param == NULL)
            this->param = strLstNew();
        else
            this->param = strLstDup(param);

        // The first parameter must be the command
        strLstInsert(this->param, 0, this->command);

        this->name = strDup(name);
        this->timeout = timeout;
    }
    MEM_CONTEXT_NEW_END();

    FUNCTION_LOG_RETURN(EXEC, this);
}

/***********************************************************************************************************************************
Execute command
***********************************************************************************************************************************/
void
execOpen(Exec *this)
{
    FUNCTION_LOG_BEGIN(logLevelDebug)
        FUNCTION_LOG_PARAM(EXEC, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    // Create pipes to communicate with the subprocess.  The names of the pipes are from the perspective of the parent process since
    // the child process will use them only briefly before exec'ing.
    int pipeRead[2];
    int pipeWrite[2];
    int pipeError[2];

    THROW_ON_SYS_ERROR(pipe(pipeRead) == -1, KernelError, "unable to create read pipe");
    THROW_ON_SYS_ERROR(pipe(pipeWrite) == -1, KernelError, "unable to create write pipe");
    THROW_ON_SYS_ERROR(pipe(pipeError) == -1, KernelError, "unable to create write pipe");

    // Fork the subprocess
    this->processId = fork();

    // Exec command in the child process
    if (this->processId == 0)
    {
        // Disable logging and close log file.  The new process will reinitialize logging if needed.
        logInit(logLevelOff, logLevelOff, logLevelOff, false);

        // Assign stdout to the input side of the read pipe and close the unused handle
        dup2(pipeRead[1], STDOUT_FILENO);
        close(pipeRead[0]);

        // Assign stdin to the output side of the write pipe and close the unused handle
        dup2(pipeWrite[0], STDIN_FILENO);
        close(pipeWrite[1]);

        // Assign stderr to the input side of the error pipe and close the unused handle
        dup2(pipeError[1], STDERR_FILENO);
        close(pipeError[0]);

        // Execute the binary.  This statement will not return if it is successful
        execvp(strPtr(this->command), (char ** const)strLstPtr(this->param));

        // If we got here then there was an error.  We can't use a throw as we normally would because we have already shutdown
        // logging and we don't want to execute exit paths that might free parent resources which we still have references to.
        fprintf(stderr, "unable to execute '%s': [%d] %s\n", strPtr(this->command), errno, strerror(errno));
        exit(errorTypeCode(&ExecuteError));
    }

    // Close the unused handles
    close(pipeRead[1]);
    close(pipeWrite[0]);
    close(pipeError[1]);

    // Store the handles we'll use and need to close when the process terminates
    this->handleRead = pipeRead[0];
    this->handleWrite = pipeWrite[1];
    this->handleError = pipeError[0];

    // Assign handles to io interfaces
    this->ioReadHandle = ioHandleReadNew(strNewFmt("%s read", strPtr(this->name)), this->handleRead, this->timeout);
    this->ioWriteHandle = ioHandleWriteIo(ioHandleWriteNew(strNewFmt("%s write", strPtr(this->name)), this->handleWrite));
    ioWriteOpen(this->ioWriteHandle);

    // Create wrapper interfaces that check process state
    this->ioReadExec = ioReadNewP(
        this, .read = (IoReadInterfaceRead)execRead, .eof = (IoReadInterfaceEof)execEof,
        .handle = (IoReadInterfaceHandle)execHandleRead);
    ioReadOpen(this->ioReadExec);
    this->ioWriteExec = ioWriteNewP(this, .write = (IoWriteInterfaceWrite)execWrite);
    ioWriteOpen(this->ioWriteExec);

    // Set a callback so the handles will get freed
    memContextCallback(this->memContext, (MemContextCallback)execFree, this);

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Check if the process is still running

This should be called when anything unexpected happens while reading or writing, including errors and eof.  If this function returns
then the original error should be rethrown.
***********************************************************************************************************************************/
static void
execCheck(Exec *this)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(EXEC, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    int processStatus;
    int processResult;

    THROW_ON_SYS_ERROR(
        (processResult = waitpid(this->processId, &processStatus, WNOHANG)) == -1, ExecuteError, "unable to wait on child process");

    if (processResult != 0)
    {
        // Clear the process id so we don't try to wait for this process on free
        this->processId = 0;

        // If the process exited normally
        if (WIFEXITED(processStatus))
        {
            // Get data from stderr to help diagnose the problem
            IoRead *ioReadError = ioHandleReadIo(ioHandleReadNew(strNewFmt("%s error", strPtr(this->name)), this->handleError, 0));
            ioReadOpen(ioReadError);
            String *errorStr = strTrim(strNewBuf(ioReadBuf(ioReadError)));

            // Throw the error with as much information as is available
            THROWP_FMT(
                errorTypeFromCode(WEXITSTATUS(processStatus)), "%s terminated unexpectedly [%d]%s%s", strPtr(this->name),
                WEXITSTATUS(processStatus), strSize(errorStr) > 0 ? ": " : "", strSize(errorStr) > 0 ? strPtr(errorStr) : "");
        }

        // If the process did not exit normally then it must have been a signal
        THROW_FMT(ExecuteError, "%s terminated unexpectedly on signal %d", strPtr(this->name), WTERMSIG(processStatus));
    }

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Read from the process
***********************************************************************************************************************************/
size_t
execRead(Exec *this, Buffer *buffer, bool block)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(EXEC, this);
        FUNCTION_LOG_PARAM(BUFFER, buffer);
        FUNCTION_LOG_PARAM(BOOL, block);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(buffer != NULL);

    size_t result = 0;

    TRY_BEGIN()
    {
        result = ioHandleRead(this->ioReadHandle, buffer, block);
    }
    CATCH_ANY()
    {
        execCheck(this);
        RETHROW();
    }
    TRY_END();

    FUNCTION_LOG_RETURN(SIZE, result);
}

/***********************************************************************************************************************************
Write to the process
***********************************************************************************************************************************/
void
execWrite(Exec *this, Buffer *buffer)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(EXEC, this);
        FUNCTION_LOG_PARAM(BUFFER, buffer);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(buffer != NULL);

    TRY_BEGIN()
    {
        ioWrite(this->ioWriteHandle, buffer);
        ioWriteFlush(this->ioWriteHandle);
    }
    CATCH_ANY()
    {
        execCheck(this);
        RETHROW();
    }
    TRY_END();

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Is the process eof?
***********************************************************************************************************************************/
bool
execEof(Exec *this)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(EXEC, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    // Check that the process is still running on eof
    if (ioHandleReadEof(this->ioReadHandle))
        execCheck(this);

    FUNCTION_LOG_RETURN(BOOL, false);
}

/***********************************************************************************************************************************
Get read interface
***********************************************************************************************************************************/
IoRead *
execIoRead(const Exec *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(EXEC, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->ioReadExec);
}

/***********************************************************************************************************************************
Get write interface
***********************************************************************************************************************************/
IoWrite *
execIoWrite(const Exec *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(EXEC, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->ioWriteExec);
}

/***********************************************************************************************************************************
Get the object mem context
***********************************************************************************************************************************/
MemContext *
execMemContext(const Exec *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(EXEC, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->memContext);
}

/***********************************************************************************************************************************
Get the read handle
***********************************************************************************************************************************/
int
execHandleRead(Exec *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(EXEC, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    FUNCTION_TEST_RETURN(this->handleRead);
}

/***********************************************************************************************************************************
Free the object
***********************************************************************************************************************************/
void
execFree(Exec *this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(EXEC, this);
    FUNCTION_TEST_END();

    if (this != NULL)
    {
        memContextCallbackClear(this->memContext);

        // Close the io handles
        close(this->handleRead);
        close(this->handleWrite);
        close(this->handleError);

        // Wait for the child to exit. We don't really care how it exits as long as it does.
        if (this->processId != 0)
        {
            MEM_CONTEXT_TEMP_BEGIN()
            {
                int processResult = 0;
                Wait *wait = waitNew(this->timeout);

                do
                {
                    THROW_ON_SYS_ERROR(
                        (processResult = waitpid(this->processId, NULL, WNOHANG)) == -1, ExecuteError,
                        "unable to wait on child process");
                }
                while (processResult == 0 && waitMore(wait));

                // If the process did not exit then error -- else we may end up with a collection of zombie processes
                if (processResult == 0)
                    THROW_FMT(ExecuteError, "%s did not exit when expected", strPtr(this->name));
            }
            MEM_CONTEXT_TEMP_END();
        }

        // Free mem context
        memContextFree(this->memContext);
    }

    FUNCTION_TEST_RETURN_VOID();
}
