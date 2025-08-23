//
// Created by Spencer Martin on 8/23/25.
//
extern "C" void *__dso_handle;
void *__dso_handle = nullptr;

extern "C" int __cxa_atexit(void (*destructor) (void *), void *arg, void *___dso_handle){
    (void)destructor;
    (void)arg;
    (void)___dso_handle;
    return 0;
}