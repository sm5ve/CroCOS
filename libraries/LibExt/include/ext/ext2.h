//
// Created by Spencer Martin on 1/27/26.
//

#ifndef CROCOS_EXT2_H
#define CROCOS_EXT2_H

#include <core/endian.h>
#include <stdint.h>

using UUIDType = LittleEndian<uint32_t>[4]; //I am presently unsure if this is the right type

struct Ext2Superblock {
    LittleEndian<uint32_t> inodes_count;
    LittleEndian<uint32_t> blocks_count;
    LittleEndian<uint32_t> r_blocks_count;
    LittleEndian<uint32_t> free_blocks_count;
    LittleEndian<uint32_t> free_inodes_count;
    LittleEndian<uint32_t> first_data_block;
    LittleEndian<uint32_t> log_block_size;
    LittleEndian<uint32_t> log_frag_size;
    LittleEndian<uint32_t> blocks_per_group;
    LittleEndian<uint32_t> frags_per_group;
    LittleEndian<uint32_t> inodes_per_group;
    LittleEndian<uint32_t> mtime;
    LittleEndian<uint32_t> wtime;
    LittleEndian<uint16_t> mnt_count;
    LittleEndian<uint16_t> max_mnt_count;
    LittleEndian<uint16_t> magic;
    LittleEndian<uint16_t> state;
    LittleEndian<uint16_t> errors;
    LittleEndian<uint16_t> minor_rev_level;
    LittleEndian<uint32_t> last_check;
    LittleEndian<uint32_t> check_interval;
    LittleEndian<uint32_t> creator_os;
    LittleEndian<uint32_t> rev_level;
    LittleEndian<uint16_t> def_resuid;
    LittleEndian<uint16_t> def_resgid;

    LittleEndian<uint32_t> first_ino;
    LittleEndian<uint16_t> inode_size;
    LittleEndian<uint16_t> block_group_nr;
    LittleEndian<uint32_t> feature_compat;
    LittleEndian<uint32_t> feature_incompat;
    LittleEndian<uint32_t> feature_ro_compat;
    UUIDType uuid;
    char volume_name[16];
    char last_mounted[64]; //I am presently unsure if this is the right type
    LittleEndian<uint32_t> algo_bitmap;

    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t rsv0;

    UUIDType journal_uuid;
    LittleEndian<uint32_t> journal_inum;
    LittleEndian<uint32_t> journal_dev;
    LittleEndian<uint32_t> last_orphan;

    LittleEndian<uint32_t> hash_seed[4];
    char def_hash_version;
    char rsv1[3];

    LittleEndian<uint32_t> default_mount_options;
    LittleEndian<uint32_t> first_meta_bg;
} __attribute__((__packed__));

static_assert(sizeof(Ext2Superblock) == 264);

#endif //CROCOS_EXT2_H