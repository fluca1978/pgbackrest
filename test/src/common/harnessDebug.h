/***********************************************************************************************************************************
C Debug Harness
***********************************************************************************************************************************/
#ifndef TEST_COMMON_HARNESS_DEBUG_H
#define TEST_COMMON_HARNESS_DEBUG_H

#ifdef NO_STACK_TRACE
    #define FUNCTION_HARNESS_INIT(exe)
    #define FUNCTION_HARNESS_BEGIN()
    #define FUNCTION_HARNESS_PARAM(typeMacroPrefix, param)
    #define FUNCTION_HARNESS_PARAM_P(typeMacroPrefix, param)
    #define FUNCTION_HARNESS_PARAM_PP(typeMacroPrefix, param)
    #define FUNCTION_HARNESS_END()
    #define FUNCTION_HARNESS_VOID()
    #define FUNCTION_HARNESS_ASSERT(condition)

    #define FUNCTION_HARNESS_RESULT(typeMacroPrefix, result)                                                                       \
        return result

    #define FUNCTION_HARNESS_RESULT_VOID();
#else
    #include "common/debug.h"

    #ifdef WITH_BACKTRACE
        #define FUNCTION_HARNESS_INIT(exe)                                                                                         \
                stackTraceInit(exe)
    #else
        #define FUNCTION_HARNESS_INIT(exe)
    #endif

    #define FUNCTION_HARNESS_BEGIN()                                                                                               \
        STACK_TRACE_PUSH(logLevelDebug);                                                                                           \
        stackTraceParamLog()

    #define FUNCTION_HARNESS_PARAM(typeMacroPrefix, param)                                                                         \
        FUNCTION_LOG_PARAM(typeMacroPrefix, param)

    #define FUNCTION_HARNESS_PARAM_P(typeMacroPrefix, param)                                                                       \
        FUNCTION_LOG_PARAM_P(typeMacroPrefix, param)

    #define FUNCTION_HARNESS_PARAM_PP(typeMacroPrefix, param)                                                                      \
        FUNCTION_LOG_PARAM_PP(typeMacroPrefix, param)

    #define FUNCTION_HARNESS_END()

    #define FUNCTION_HARNESS_VOID()                                                                                                \
        FUNCTION_HARNESS_BEGIN();                                                                                                  \
        FUNCTION_HARNESS_END()

    #define FUNCTION_HARNESS_ASSERT(condition)                                                                                     \
        do                                                                                                                         \
        {                                                                                                                          \
            if (!(condition))                                                                                                      \
                THROW_FMT(AssertError, "function harness assertion '%s' failed", #condition);                                      \
        }                                                                                                                          \
        while (0)

    #define FUNCTION_HARNESS_RESULT(typeMacroPrefix, result)                                                                       \
        STACK_TRACE_POP();                                                                                                         \
        return result                                                                                                              \

    #define FUNCTION_HARNESS_RESULT_VOID()                                                                                         \
        STACK_TRACE_POP();
#endif

#endif
