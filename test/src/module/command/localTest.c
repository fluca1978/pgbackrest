/***********************************************************************************************************************************
Test Local Command
***********************************************************************************************************************************/
#include "common/io/handleRead.h"
#include "common/io/handleWrite.h"
#include "protocol/client.h"
#include "protocol/server.h"

#include "common/harnessConfig.h"
#include "common/harnessFork.h"

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    // *****************************************************************************************************************************
    if (testBegin("cmdLocal()"))
    {
        // Create pipes for testing.  Read/write is from the perspective of the client.
        int pipeRead[2];
        int pipeWrite[2];
        THROW_ON_SYS_ERROR(pipe(pipeRead) == -1, KernelError, "unable to read test pipe");
        THROW_ON_SYS_ERROR(pipe(pipeWrite) == -1, KernelError, "unable to write test pipe");

        HARNESS_FORK_BEGIN()
        {
            HARNESS_FORK_CHILD_BEGIN(0, true)
            {
                StringList *argList = strLstNew();
                strLstAddZ(argList, "pgbackrest");
                strLstAddZ(argList, "--stanza=test1");
                strLstAddZ(argList, "--command=archive-get-async");
                strLstAddZ(argList, "--process=1");
                strLstAddZ(argList, "--type=backup");
                strLstAddZ(argList, "--host-id=1");
                strLstAddZ(argList, "local");
                harnessCfgLoad(strLstSize(argList), strLstPtr(argList));

                cmdLocal(HARNESS_FORK_CHILD_READ(), HARNESS_FORK_CHILD_WRITE());
            }
            HARNESS_FORK_CHILD_END();

            HARNESS_FORK_PARENT_BEGIN()
            {
                IoRead *read = ioHandleReadIo(ioHandleReadNew(strNew("server read"), HARNESS_FORK_PARENT_READ_PROCESS(0), 2000));
                ioReadOpen(read);
                IoWrite *write = ioHandleWriteIo(ioHandleWriteNew(strNew("server write"), HARNESS_FORK_PARENT_WRITE_PROCESS(0)));
                ioWriteOpen(write);

                ProtocolClient *client = protocolClientNew(strNew("test"), PROTOCOL_SERVICE_LOCAL_STR, read, write);
                protocolClientNoOp(client);
                protocolClientFree(client);
            }
            HARNESS_FORK_PARENT_END();
        }
        HARNESS_FORK_END();
    }

    FUNCTION_HARNESS_RESULT_VOID();
}
