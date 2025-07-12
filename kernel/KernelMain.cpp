//
// Created by Spencer Martin on 2/13/25.
//
#include <arch/hal/hal.h>
#include <kernel.h>
#include <core/atomic.h>
#include <core/Object.h>

extern "C" void (*__init_array_start[])(void) __attribute__((weak));
extern "C" void (*__init_array_end[])(void) __attribute__((weak));

namespace kernel{
    hal::SerialPrintStream EarlyBootStream;
    Core::PrintStream& klog = EarlyBootStream;

    WITH_GLOBAL_CONSTRUCTOR(Spinlock, lock);

    void run_global_constructors(){
        for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ctor++) {
            (*ctor)();
        }
    }

    CRClass(A) {

    };

    CRClass(B) {

    };

    CRClass(C, public A, public B) {

    };

    extern "C" void kernel_main(){
        klog << "\n"; // newline to separate from the "Booting from ROM.." message from qemu

        klog << "Hello amd64 kernel world!\n";

        presort_object_parent_lists();
        run_global_constructors();

        A* a = new C();
        (void)a;

        klog << ( a -> instanceof(TypeID_v<B>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<A>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<C>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<int>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<ObjectBase>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<Vector<int>>)) << "\n";
        klog << ( a -> instanceof(TypeID_v<Vector<bool>>)) << "\n";

        klog << a -> type_name() << "\n";
        klog << a << "\n";
        auto b = crocos_dynamic_cast<B*>(a);
        klog << b << "\n";
        klog << crocos_dynamic_cast<A*>(b) << "\n";

        hal::hwinit();

        klog << "init done!\n";

        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}