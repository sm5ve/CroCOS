//
// Created by Spencer Martin on 1/8/26.
//

#ifndef CROCOS_AMD64INTERNAL_H
#define CROCOS_AMD64INTERNAL_H
namespace kernel::amd64 {
    void unmapIdentity();
    void remapIdentity();
    void enableFSGSBase();
}
#endif //CROCOS_AMD64INTERNAL_H