#ifndef LOCACHY_KVM_DECODE_H
#define LOCACHY_KVM_DECODE_H
#include "kvm-protocol.h"
struct kvm_decoder;
/* A callback borrows native-endian 0x00RRGGBB pixels until it returns. */
typedef int (*kvm_pixels_fn)(void *, const uint32_t *, uint32_t, uint32_t);
int kvm_decoder_available(enum kvm_codec codec);
struct kvm_decoder *kvm_decoder_open(enum kvm_codec codec, uint32_t width, uint32_t height);
int kvm_decoder_push(struct kvm_decoder *, const void *, size_t, kvm_pixels_fn, void *);
void kvm_decoder_close(struct kvm_decoder *);
#endif
