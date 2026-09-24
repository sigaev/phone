/* Startup objects can contain unwind tables even when our C++ disables them. */
SECTIONS {
    /DISCARD/ : {
        *(.eh_frame .eh_frame.* .eh_frame_hdr)
        *(.gcc_except_table .gcc_except_table.*)
        *(.ARM.exidx* .ARM.extab*)
    }
}
INSERT AFTER .rodata;

/* Reject accidental reintroduction of an exception runtime or demangler. */
ASSERT(!DEFINED(__cxa_throw), "C++ exceptions are disabled")
ASSERT(!DEFINED(__cxa_allocate_exception), "C++ exception allocation is disabled")
ASSERT(!DEFINED(__cxa_begin_catch), "C++ exception catching is disabled")
ASSERT(!DEFINED(__gxx_personality_v0), "C++ exception personalities are disabled")
ASSERT(!DEFINED(_Unwind_RaiseException), "Stack unwinding is disabled")
ASSERT(!DEFINED(_Unwind_Resume), "Stack unwinding is disabled")
ASSERT(!DEFINED(__cxa_demangle), "C++ name demangling is disabled")
