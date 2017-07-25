/**
 * Copyright (C) 2017 Cisco Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// @author Changxue Deng <chadeng@cisco.com>

#include <stdlib.h>
#include <iostream>
#include <errno.h>

#include "mabain_consts.h"
#include "db.h"
#include "dict.h"
#include "dict_mem.h"
#include "error.h"
#include "integer_4b_5b.h"

#define MAX_DATA_BUFFER_RESERVE_SIZE    0xFFFF
#define NUM_DATA_BUFFER_RESERVE         MAX_DATA_BUFFER_RESERVE_SIZE/DATA_BUFFER_ALIGNMENT

#define READER_LOCK_FREE_START               \
    LockFreeData snapshot;                   \
    int lf_ret;                              \
    LockFree::ReaderLockFreeStart(snapshot);
#define READER_LOCK_FREE_STOP(edgeoff)                          \
    lf_ret = LockFree::ReaderLockFreeStop(snapshot, (edgeoff)); \
    if(lf_ret != MBError::SUCCESS)                              \
        return lf_ret;

namespace mabain {

Dict::Dict(const std::string &mbdir, bool init_header, int datasize,
           int db_options, size_t memsize_index, size_t memsize_data,
           bool use_sliding_map, bool sync_on_write)
         : mb_dir(mbdir),
           options(db_options),
           mm(mbdir, init_header, memsize_index, use_sliding_map,
              db_options, sync_on_write),
           free_lists(NULL),
           header(NULL)
{
    status = MBError::NOT_INITIALIZED;

    // Open data file
    db_file = new RollableFile(mbdir + "_mabain_d",
              static_cast<size_t>(DATA_BLOCK_SIZE), memsize_data,
              use_sliding_map, db_options, sync_on_write);

    header = mm.GetHeaderPtr();
    if(header == NULL)
    {
        Logger::Log(LOG_LEVEL_ERROR, "header not mapped");
        return;
    }

    // If init_header is false, we can set the dict status to SUCCESS.
    // Otherwise, the status will be set in the Init.
    if(init_header)
    {
        // Initialize header
        header->data_size = datasize;
        header->count = 0;
        header->m_data_offset = GetStartDataOffset(); // start from a non-zero offset
        // We known that only writers will set init_header to true.
        free_lists = new FreeList(mbdir+"_dbfl", DATA_BUFFER_ALIGNMENT,
                                  NUM_DATA_BUFFER_RESERVE);
    }
    else
    {
        if(options & CONSTS::ACCESS_MODE_WRITER)
        {
            free_lists = new FreeList(mbdir+"_dbfl", DATA_BUFFER_ALIGNMENT,
                                      NUM_DATA_BUFFER_RESERVE);
            int rval = free_lists->LoadListFromDisk();
            if(rval == MBError::SUCCESS)
            {
                if(mm.IsValid())
                    status = MBError::SUCCESS;
            }
            else
            {
                Logger::Log(LOG_LEVEL_ERROR, "failed to load data free list from disk %s",
                            MBError::get_error_str(rval));
            }
        }
        else
        {
            if(mm.IsValid())
                status = MBError::SUCCESS;
        }
    }
}

Dict::~Dict()
{
}

// This function only needs to be called by writer.
int Dict::Init(uint32_t id)
{
    if(!(options & CONSTS::ACCESS_MODE_WRITER))
    {
        Logger::Log(LOG_LEVEL_ERROR, "dict initialization not allowed for non-writer");
        return MBError::NOT_ALLOWED;
    }

    if(status != MBError::NOT_INITIALIZED)
    {
        // status can be NOT_INITIALIZED or SUCCESS.
        Logger::Log(LOG_LEVEL_WARN, "connector %u dict already initialized", id);
        return MBError::SUCCESS;
    }

    if(header == NULL)
    {
        Logger::Log(LOG_LEVEL_ERROR, "connector %u header not mapped", id);
        return MBError::ALLOCATION_ERROR;
    }

    Logger::Log(LOG_LEVEL_INFO, "connector %u initializing DictMem", id);
    mm.InitRootNode();

    if(header->data_size > CONSTS::MAX_DATA_SIZE)
    {
        Logger::Log(LOG_LEVEL_ERROR, "data size %d is too large", header->data_size);
        return MBError::INVALID_SIZE;
    }

    if(mm.IsValid())
        status = MBError::SUCCESS;

    return status;
}

void Dict::Destroy()
{
    mm.Destroy();

    // Dump free list to disk
    if(free_lists)
    {
        int rval = free_lists->StoreListOnDisk();
        if(rval != MBError::SUCCESS)
            Logger::Log(LOG_LEVEL_ERROR, "failed to dump free list to disk %s",
                                     MBError::get_error_str(rval));
        delete free_lists;
    }

    if(db_file)
        delete db_file;
}

int Dict::Status() const
{
    return status;
}

int Dict::GetDBOptions() const
{
    return options;
}

// Add a key-value pair
// if overwrite is true and an entry with input key already exists, the old data will
// be overwritten. Otherwise, IN_DICT will be returned.
int Dict::Add(const uint8_t *key, int len, MBData &data, bool overwrite)
{
    if(!(options & CONSTS::ACCESS_MODE_WRITER))
        return MBError::NOT_ALLOWED;
    if(len > CONSTS::MAX_KEY_LENGHTH || data.data_len > CONSTS::MAX_DATA_SIZE)
        return MBError::OUT_OF_BOUND;

    EdgePtrs edge_ptrs;
    int rval = mm.GetRootEdge(key[0], edge_ptrs);
    size_t data_offset = 0;
    if(rval != MBError::SUCCESS)
        return rval;

    if(edge_ptrs.len_ptr[0] == 0)
    {
        ReserveData(data.buff, data.data_len, data_offset);
        // Add the first edge along this edge
        mm.AddRootEdge(edge_ptrs, key, len, data_offset);
        header->count++;
        return MBError::SUCCESS;
    }

    bool inc_count = true;
    int i;
    const uint8_t *key_buff;
    uint8_t tmp_key_buff[NUM_ALPHABET];
    const uint8_t *p = key;
    int edge_len = edge_ptrs.len_ptr[0];
    if(edge_len > LOCAL_EDGE_LEN)
    {
        if(mm.ReadData(tmp_key_buff, edge_len-1, Get5BInteger(edge_ptrs.ptr), true)
                      != edge_len-1)
            return MBError::READ_ERROR;
        key_buff = tmp_key_buff;
    }
    else
    {
        key_buff = edge_ptrs.ptr;
    }
    if(edge_len < len)
    {
        for(i = 1; i < edge_len; i++)
        {
            if(key_buff[i-1] != key[i])
                break;
        }
        if(i >= edge_len)
        {
            int match_len;
            bool next;
            p += edge_len;
            len -= edge_len;
            while((next = mm.FindNext(p, len, match_len, edge_ptrs, tmp_key_buff)))
            {
                if(match_len < edge_ptrs.len_ptr[0])
                    break;

                p += match_len;
                len -= match_len;
                if(len <= 0)
                    break;
            }
            if(!next)
            {
                ReserveData(data.buff, data.data_len, data_offset);
                rval = mm.UpdateNode(edge_ptrs, p, len, data_offset);
            }
            else if(match_len < static_cast<int>(edge_ptrs.len_ptr[0]))
            {
                if(len > match_len)
                {
                    ReserveData(data.buff, data.data_len, data_offset);
                    rval = mm.AddLink(edge_ptrs, match_len, p+match_len, len-match_len,
                                      data_offset, data);
                }
                else if(len == match_len)
                {
                    ReserveData(data.buff, data.data_len, data_offset);
                    rval = mm.InsertNode(edge_ptrs, match_len, data_offset, data);
                }
            }
            else if(len == 0)
            {
                rval = UpdateDataBuffer(edge_ptrs, overwrite, data.buff, data.data_len, inc_count);
            }
        }
        else
        {
            ReserveData(data.buff, data.data_len, data_offset);
            rval = mm.AddLink(edge_ptrs, i, p+i, len-i, data_offset, data);
        }
    }
    else
    {
        for(i = 1; i < len; i++)
        {
            if(key_buff[i-1] != key[i])
                break;
        }
        if(i < len)
        {
            ReserveData(data.buff, data.data_len, data_offset);
            rval = mm.AddLink(edge_ptrs, i, p+i, len-i, data_offset, data);
        }
        else
        {
            if(edge_ptrs.len_ptr[0] > len)
            {
                ReserveData(data.buff, data.data_len, data_offset);
                rval = mm.InsertNode(edge_ptrs, i, data_offset, data);
            }
            else
            {
                rval = UpdateDataBuffer(edge_ptrs, overwrite, data.buff, data.data_len, inc_count);
            }
        }
    }

    if(inc_count)
        header->count++;
    return rval;
}

int Dict::ReadDataFromEdge(MBData &data, const EdgePtrs &edge_ptrs) const
{
    size_t data_off;
    if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
    {
        data_off = Get6BInteger(edge_ptrs.offset_ptr);
    }
    else
    {
        uint8_t node_buff[NODE_EDGE_KEY_FIRST];
        if(mm.ReadData(node_buff, NODE_EDGE_KEY_FIRST, Get6BInteger(edge_ptrs.offset_ptr))
                      != NODE_EDGE_KEY_FIRST)
            return MBError::READ_ERROR;
        if(!(node_buff[0] & FLAG_NODE_MATCH))
            return MBError::NOT_EXIST;
        data_off = Get6BInteger(node_buff+2);
    }
    data.data_offset = data_off;

    uint16_t data_len;
    // Read data length first
    if(ReadData(reinterpret_cast<uint8_t*>(&data_len), DATA_SIZE_BYTE, data_off)
               != DATA_SIZE_BYTE)
        return MBError::READ_ERROR;
    data_off += DATA_SIZE_BYTE;
    if(data.buff_len < data_len)
    {
        if(data.Resize(data_len) != MBError::SUCCESS)
            return MBError::NO_MEMORY;
    }
    if(ReadData(data.buff, data_len, data_off) != data_len)
        return MBError::READ_ERROR;

    data.data_len = data_len;
    return MBError::SUCCESS;
}

// Delete operations:
//   If this is a leaf node, need to remove the edge. Otherwise, unset the match flag.
//   Also need to set the delete flag in the data block so that it can be reclaimed later.
int Dict::DeleteDataFromEdge(MBData &data, EdgePtrs &edge_ptrs)
{
    int rval = MBError::SUCCESS;
    size_t data_off;
    uint16_t data_len;
    int rel_size;

    // Check if this is a leaf node first by using the EDGE_FLAG_DATA_OFF bit
    if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
    {
        data_off = Get6BInteger(edge_ptrs.offset_ptr);
        if(ReadData(reinterpret_cast<uint8_t*>(&data_len), DATA_SIZE_BYTE, data_off, true)
                   != DATA_SIZE_BYTE)
            return MBError::READ_ERROR;

        rel_size = free_lists->GetAlignmentSize(data_len + DATA_SIZE_BYTE);
        header->pending_data_buff_size += rel_size;
        free_lists->ReleaseBuffer(data_off, rel_size);

        rval = mm.RemoveEdgeByIndex(edge_ptrs, data);
    }
    else
    {
        uint8_t node_buff[NODE_EDGE_KEY_FIRST];
        size_t node_off = Get6BInteger(edge_ptrs.offset_ptr);

        // Read node header
        if(mm.ReadData(node_buff, NODE_EDGE_KEY_FIRST, node_off, true) != NODE_EDGE_KEY_FIRST)
            return MBError::READ_ERROR;

        if(node_buff[0] & FLAG_NODE_MATCH)
        {
            // Unset the match flag
            node_buff[0] &= ~FLAG_NODE_MATCH;
            mm.WriteData(&node_buff[0], 1, node_off);

            // Release data buffer
            data_off = Get6BInteger(node_buff+2);
            if(ReadData(reinterpret_cast<uint8_t*>(&data_len), DATA_SIZE_BYTE, data_off, true)
                       != DATA_SIZE_BYTE)
                return MBError::READ_ERROR;

            rel_size = free_lists->GetAlignmentSize(data_len + DATA_SIZE_BYTE);
            header->pending_data_buff_size += rel_size;
            free_lists->ReleaseBuffer(data_off, rel_size);
        }
    }

    return rval;
}

int Dict::ReadDataFromNode(MBData &data, const uint8_t *node_ptr) const
{
    size_t data_off = Get6BInteger(node_ptr+2);
    if(data_off == 0)
        return MBError::NOT_EXIST;

    data.data_offset = data_off;

    // Read data length first
    uint16_t data_len;
    if(ReadData(reinterpret_cast<uint8_t *>(&data_len), DATA_SIZE_BYTE, data_off)
               != DATA_SIZE_BYTE)
        return MBError::READ_ERROR;
    data_off += DATA_SIZE_BYTE;

    if(data.buff_len < data_len)
    {
        if(data.Resize(data_len) != MBError::SUCCESS)
            return MBError::NO_MEMORY;
    }
    if(ReadData(data.buff, data_len, data_off) != data_len)
        return MBError::READ_ERROR;

    data.data_len = data_len;
    return MBError::SUCCESS;
}

int Dict::FindPrefix(const uint8_t *key, int len, MBData &data) const
{
    data.next = false;
    EdgePtrs &edge_ptrs = data.edge_ptrs;
#ifdef __LOCK_FREE__
    READER_LOCK_FREE_START
#endif

    if(data.match_len == 0)
    {
        if(mm.GetRootEdge(key[0], edge_ptrs) != MBError::SUCCESS)
            return MBError::READ_ERROR;

        if(edge_ptrs.len_ptr[0] == 0)
        {
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            return MBError::NOT_EXIST;
        }
    }

    // Compare edge string
    const uint8_t *key_buff;
    uint8_t *node_buff = data.node_buff;
    const uint8_t *p = key;
    int edge_len = edge_ptrs.len_ptr[0];
    int edge_len_m1 = edge_len - 1;
    if(edge_len > LOCAL_EDGE_LEN)
    {
        if(mm.ReadData(node_buff, edge_len_m1, Get5BInteger(edge_ptrs.ptr))
                      != edge_len_m1)
        {
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            return MBError::READ_ERROR;
        }
        key_buff = node_buff;
    }
    else
    {
        key_buff = edge_ptrs.ptr;
    }

    int rval = MBError::NOT_EXIST;
    if(edge_len < len)
    {
        if(edge_len > 1 && memcmp(key_buff, key+1, edge_len_m1) != 0)
        {
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            return MBError::NOT_EXIST;
        }

        len -= edge_len;
        p += edge_len;

        if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
        {
            // prefix match for leaf node
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            data.match_len = p - key;
            return ReadDataFromEdge(data, edge_ptrs);
        }

        uint8_t last_node_buffer[NODE_EDGE_KEY_FIRST];
#ifdef __LOCK_FREE__
        size_t edge_offset_prev = edge_ptrs.offset;
#endif
        int last_prefix_rval = MBError::NOT_EXIST;
        while(true)
        {
            rval = mm.NextEdge(p, edge_ptrs, node_buff);
            if(rval != MBError::READ_ERROR)
            {
                if(node_buff[0] & FLAG_NODE_MATCH)
                {
                    data.match_len = p - key;
                    if(data.options & CONSTS::OPTION_ALL_PREFIX)
                    {
                        rval = ReadDataFromNode(data, node_buff);
                        data.next = true;
                        rval = last_prefix_rval;
                        break;
                    }
                    else
                    {
                        memcpy(last_node_buffer, node_buff, NODE_EDGE_KEY_FIRST);
                        last_prefix_rval = MBError::SUCCESS;
                    }
                }
            }

            if(rval != MBError::SUCCESS)
                break;

#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_offset_prev)
#endif
            edge_len = edge_ptrs.len_ptr[0];
            edge_len_m1 = edge_len - 1;
            // match edge string
            if(edge_len > LOCAL_EDGE_LEN)
            {
                if(mm.ReadData(node_buff, edge_len_m1, Get5BInteger(edge_ptrs.ptr))
                              != edge_len_m1)
                {
                    rval = MBError::READ_ERROR;
                    break;
                }
                key_buff = node_buff;
            }
            else
            {
                key_buff = edge_ptrs.ptr;
            }

            if(edge_len > 1 && memcmp(key_buff, p+1, edge_len_m1) != 0)
            {
                rval = MBError::NOT_EXIST;
                break;
            }

            len -= edge_len;
            p += edge_len;
            if(len <= 0 || (edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF))
            {
                data.match_len = p - key;
                rval = ReadDataFromEdge(data, edge_ptrs);
                break;
            }
#ifdef __LOCK_FREE__
            edge_offset_prev = edge_ptrs.offset;
#endif
        }

        if(rval == MBError::NOT_EXIST && last_prefix_rval != rval)
            rval = ReadDataFromNode(data, last_node_buffer);
    }
    else if(edge_len == len)
    {
        if(memcmp(key_buff, key+1, edge_len_m1) == 0)
        {
            data.match_len = len;
            rval = ReadDataFromEdge(data, edge_ptrs);
        }
    }

#ifdef __LOCK_FREE__
    READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
    return rval;
}

int Dict::Find(const uint8_t *key, int len, MBData &data) const
{
    EdgePtrs &edge_ptrs = data.edge_ptrs;
#ifdef __LOCK_FREE__
    READER_LOCK_FREE_START
#endif
    if(mm.GetRootEdge(key[0], edge_ptrs) != MBError::SUCCESS)
        return MBError::READ_ERROR;
    if(edge_ptrs.len_ptr[0] == 0)
    {
#ifdef __LOCK_FREE__
        READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
        return MBError::NOT_EXIST;
    }

    // Compare edge string
    const uint8_t *key_buff;
    uint8_t *node_buff = data.node_buff;
    const uint8_t *p = key;
    int edge_len = edge_ptrs.len_ptr[0];
    int edge_len_m1 = edge_len - 1;
    int rval = MBError::NOT_EXIST;

    if(edge_len > LOCAL_EDGE_LEN)
    {
        size_t edge_str_off_lf = Get5BInteger(edge_ptrs.ptr);
        if(mm.ReadData(node_buff, edge_len_m1, edge_str_off_lf) != edge_len_m1)
        {
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            return MBError::READ_ERROR;
        }
        key_buff = node_buff;
    }
    else
    {
        key_buff = edge_ptrs.ptr;
    }

    if(edge_len < len)
    {
        if((edge_len > 1 && memcmp(key_buff, key+1, edge_len_m1) != 0) ||
           (edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF))
        {
#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
            return MBError::NOT_EXIST;
        }

        len -= edge_len;
        p += edge_len;

#ifdef __LOCK_FREE__
        size_t edge_offset_prev = edge_ptrs.offset;
#endif
        while(true)
        {
            rval = mm.NextEdge(p, edge_ptrs, node_buff, data.options & CONSTS::OPTION_FIND_AND_DELETE);
            if(rval != MBError::SUCCESS)
                break;

#ifdef __LOCK_FREE__
            READER_LOCK_FREE_STOP(edge_offset_prev)
#endif
            edge_len = edge_ptrs.len_ptr[0];
            edge_len_m1 = edge_len - 1;
            // match edge string
            if(edge_len > LOCAL_EDGE_LEN)
            {
                size_t edge_str_off_lf = Get5BInteger(edge_ptrs.ptr);
                if(mm.ReadData(node_buff, edge_len_m1, edge_str_off_lf) != edge_len_m1)
                {
                    rval = MBError::READ_ERROR;
                    break;
                }
                key_buff = node_buff;
            }
            else
            {
                key_buff = edge_ptrs.ptr;
            }

            if(edge_len_m1 > 0 && memcmp(key_buff, p+1, edge_len_m1) != 0)
            {
                rval = MBError::NOT_EXIST;
                break;
            }

            len -= edge_len;
            if(len <= 0)
            {
                // If this is for remove operation, return IN_DICT to caller.
                if(data.options & CONSTS::OPTION_FIND_AND_DELETE)
                    return MBError::IN_DICT;

                rval =  ReadDataFromEdge(data, edge_ptrs);
                break;
            }
            else
            {
                if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
                {
                    // Reach a leaf node and no match found
                    rval = MBError::NOT_EXIST;
                    break;
                }
            }
            p += edge_len;
#ifdef __LOCK_FREE__
            edge_offset_prev = edge_ptrs.offset;
#endif
        }
    }
    else if(edge_len == len)
    {
        if(memcmp(key_buff, key+1, len-1) != 0)
        {
            rval = MBError::NOT_EXIST;
        }
        else
        {
            // If this is for remove operation, return IN_DICT to caller.
            if(data.options & CONSTS::OPTION_FIND_AND_DELETE)
            {
                data.edge_ptrs.curr_node_offset = mm.GetRootOffset();
                data.edge_ptrs.curr_nt = 1;
                data.edge_ptrs.curr_edge_index = 0;
                data.edge_ptrs.parent_offset = data.edge_ptrs.offset;
                return MBError::IN_DICT;
            }
            rval = ReadDataFromEdge(data, edge_ptrs);
        }
    }

#ifdef __LOCK_FREE__
    READER_LOCK_FREE_STOP(edge_ptrs.offset)
#endif
    return rval;
}

void Dict::PrintStats(std::ostream *out_stream) const
{
    if(out_stream != NULL)
        return PrintStats(*out_stream);
    return PrintStats(std::cout);
}

void Dict::PrintStats(std::ostream &out_stream) const
{
    if(status != MBError::SUCCESS)
        return;

    out_stream << "DB stats:\n";
    out_stream << "\tNumer of DB writer: " << header->num_writer << std::endl;
    out_stream << "\tNumer of DB reader: " << header->num_reader << std::endl;
    out_stream << "\tEntry count in DB " + mb_dir + ": " << header->count << std::endl;
    out_stream << "\tData size: " << header->m_data_offset << std::endl;
    if(free_lists)
    {
        out_stream << "\tPending Buffer Size: " << header->pending_data_buff_size << "\n";
        out_stream << "\tTrackable Buffer Size: " << free_lists->GetTotSize() << "\n";
        out_stream << "\tResidual Buffer Size: " <<
            header->m_data_offset - header->pending_data_buff_size - GetStartDataOffset() << "\n";
    }
    mm.PrintStats(out_stream);

    db_file->PrintStats(out_stream);
}

int64_t Dict::Count() const
{
    if(header == NULL)
    {
        Logger::Log(LOG_LEVEL_WARN, "db was not initialized successfully: %s",
                    MBError::get_error_str(status));
        return 0;
    }

    return header->count;
}

// For DB iterator
int Dict::ReadNextEdge(const uint8_t *node_buff, EdgePtrs &edge_ptrs,
                       int &match, MBData &data, std::string &match_str,
                       size_t &node_off) const
{
    if(edge_ptrs.curr_nt > static_cast<int>(node_buff[1]))
        return MBError::OUT_OF_BOUND;

    if(mm.ReadData(edge_ptrs.edge_buff, EDGE_SIZE, edge_ptrs.offset) != EDGE_SIZE)
        return MBError::READ_ERROR;

    node_off = 0;

    int rval = MBError::SUCCESS;
    InitTempEdgePtrs(edge_ptrs);
    if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
    {
        // match of leaf node
        match = MATCH_EDGE;
        rval = ReadDataFromEdge(data, edge_ptrs);
        if(rval != MBError::SUCCESS)
            return rval;
    }
    else
    {
        match = MATCH_NONE;
        node_off = Get6BInteger(edge_ptrs.offset_ptr);
    }
    edge_ptrs.parent_offset = edge_ptrs.offset;

    int edge_len_m1 = edge_ptrs.len_ptr[0] - 1;
    match_str = std::string(1, (const char)node_buff[NODE_EDGE_KEY_FIRST+edge_ptrs.curr_nt]);
    if(edge_len_m1 > LOCAL_EDGE_LEN_M1)
    {
        if(mm.ReadData(data.node_buff, edge_len_m1, Get5BInteger(edge_ptrs.ptr)) != edge_len_m1)
            return MBError::READ_ERROR;
        match_str += std::string(reinterpret_cast<char*>(data.node_buff), edge_len_m1);
    }
    else if(edge_len_m1 > 0)
    {
        match_str += std::string(reinterpret_cast<char*>(edge_ptrs.ptr), edge_len_m1);
    }

    edge_ptrs.curr_nt++;
    edge_ptrs.offset += EDGE_SIZE;
    return rval;
}

// For DB iterator
int Dict::ReadNode(size_t node_off, uint8_t *node_buff, EdgePtrs &edge_ptrs,
                   int &match, MBData &data) const
{
    if(mm.ReadData(node_buff, NODE_EDGE_KEY_FIRST, node_off) != NODE_EDGE_KEY_FIRST)
        return MBError::READ_ERROR;

    edge_ptrs.curr_nt = 0;
    int nt = node_buff[1] + 1;
    node_off += NODE_EDGE_KEY_FIRST;
    if(mm.ReadData(node_buff + NODE_EDGE_KEY_FIRST, nt, node_off) != nt)
        return MBError::READ_ERROR;

    int rval = MBError::SUCCESS;
    edge_ptrs.offset = node_off + nt;
    if(node_buff[0] & FLAG_NODE_MATCH)
    {
        // match of non-leaf node
        match = MATCH_NODE;
        rval = ReadDataFromNode(data, node_buff);
    }
    else
    {
        // no match at the non-leaf node
        match = MATCH_NONE;
    }

    return rval;
}

size_t Dict::GetRootOffset() const
{
    return mm.GetRootOffset();
}

// For DB iterator
int Dict::ReadRootNode(uint8_t *node_buff, EdgePtrs &edge_ptrs, int &match,
                       MBData &data) const
{
    return ReadNode(mm.GetRootOffset(), node_buff, edge_ptrs, match, data);
}

int Dict::Remove(const uint8_t *key, int len)
{
    MBData data(0, CONSTS::OPTION_FIND_AND_DELETE);
    return Remove(key, len, data);
}

int Dict::Remove(const uint8_t *key, int len, MBData &data)
{
    if(!(options & CONSTS::ACCESS_MODE_WRITER))
        return MBError::NOT_ALLOWED;

    // The DELETE flag must be set
    if(!(data.options & CONSTS::OPTION_FIND_AND_DELETE))
        return MBError::INVALID_ARG;

    int rval;
    rval = Find(key, len, data);
    if(rval == MBError::IN_DICT)
    {
        rval = DeleteDataFromEdge(data, data.edge_ptrs);
        while(rval == MBError::TRY_AGAIN)
        {
            data.Clear();
            len -= data.edge_ptrs.len_ptr[0];
            assert(len > 0);
            rval = Find(key, len, data);
            if(MBError::IN_DICT == rval)
            {
                rval = mm.RemoveEdgeByIndex(data.edge_ptrs, data);
            }
        }
         
    }

    if(rval == MBError::SUCCESS)
        header->count--;
    return rval;
}

int Dict::RemoveAll()
{
    int rval = MBError::SUCCESS;;
    for(int c = 0; c < NUM_ALPHABET; c++)
    { 
        rval = mm.ClearRootEdge(c);
        if(rval != MBError::SUCCESS)
            break;
    }

    mm.ClearMem();
    header->count = 0;
    header->m_data_offset = GetStartDataOffset();
    free_lists->Empty();
    header->pending_data_buff_size = 0;
    return rval;
}

void Dict::SetShmLockPtrs() const
{
    MBLock::Init(&header->mb_rw_lock);
}

int Dict::InitShmMutex()
{
    if(status != MBError::SUCCESS)
        return MBError::NOT_INITIALIZED;

    Logger::Log(LOG_LEVEL_INFO, "initializing shared memory mutex");

    // Reset set status to MUTEX_ERROR
    status = MBError::MUTEX_ERROR;

    pthread_rwlockattr_t attr;
    if(pthread_rwlockattr_init(&attr))
    {
        Logger::Log(LOG_LEVEL_WARN, "pthread_rwlockattr_init failed");
        return MBError::MUTEX_ERROR;
    }
    if(pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED))
    {
        Logger::Log(LOG_LEVEL_WARN, "failed to set PTHREAD_PROCESS_SHARED");
        pthread_rwlockattr_destroy(&attr);
        return MBError::MUTEX_ERROR;
    }

    if(pthread_rwlock_init(&header->mb_rw_lock, &attr))
    {
        Logger::Log(LOG_LEVEL_WARN, "pthread_rwlock_init failed");
        pthread_rwlockattr_destroy(&attr);
        return MBError::MUTEX_ERROR;
    }
    pthread_rwlockattr_destroy(&attr);

    // Set status back to OK
    status = MBError::SUCCESS;
    return MBError::SUCCESS;
}

// Reserve buffer and write to it
void Dict::ReserveData(const uint8_t* buff, int size, size_t &offset)
{
    assert(size <= CONSTS::MAX_DATA_SIZE);

    int buf_size  = free_lists->GetAlignmentSize(size + DATA_SIZE_BYTE);
    int buf_index = free_lists->GetBufferIndex(buf_size);
    uint16_t dsize = static_cast<uint16_t>(size);

    if(free_lists->GetBufferCountByIndex(buf_index) > 0)
    {
        offset = free_lists->RemoveBufferByIndex(buf_index);
        WriteData(reinterpret_cast<const uint8_t*>(&dsize), DATA_SIZE_BYTE, offset);
        WriteData(buff, size, offset+DATA_SIZE_BYTE);
        header->pending_data_buff_size -= buf_size;
    }
    else
    {
        size_t old_off = header->m_data_offset;
        uint8_t *ptr;

        int rval = db_file->Reserve(header->m_data_offset, buf_size, ptr);
        if(rval != MBError::SUCCESS)
            throw rval;

        //Checking missing buffer due to alignment
        if(old_off < header->m_data_offset)
        {
            free_lists->ReleaseAlignmentBuffer(old_off, header->m_data_offset);
            header->pending_data_buff_size += header->m_data_offset - old_off;
        }

        offset = header->m_data_offset;
        header->m_data_offset += buf_size;
        if(ptr != NULL)
        {
            memcpy(ptr, &dsize, DATA_SIZE_BYTE);
            memcpy(ptr+DATA_SIZE_BYTE, buff, size);
        }
        else
        {
            WriteData(reinterpret_cast<const uint8_t*>(&dsize), DATA_SIZE_BYTE, offset);
            WriteData(buff, size, offset+DATA_SIZE_BYTE);
        }
    }
}

int Dict::ReleaseBuffer(size_t offset)
{
    uint16_t data_size;

    if(ReadData(reinterpret_cast<uint8_t*>(&data_size), DATA_SIZE_BYTE, offset, true)
               != DATA_SIZE_BYTE)
        return MBError::READ_ERROR;

    int rel_size = free_lists->GetAlignmentSize(data_size + DATA_SIZE_BYTE);
    header->pending_data_buff_size += rel_size;
    return free_lists->ReleaseBuffer(offset, rel_size);
}

int Dict::UpdateDataBuffer(EdgePtrs &edge_ptrs, bool overwrite, const uint8_t *buff,
                           int len, bool &inc_count)
{
    size_t data_off;

    if(edge_ptrs.flag_ptr[0] & EDGE_FLAG_DATA_OFF)
    {
        inc_count = false;
        // leaf node
        if(!overwrite)
            return MBError::IN_DICT;

        data_off = Get6BInteger(edge_ptrs.offset_ptr);
        if(ReleaseBuffer(data_off) != MBError::SUCCESS)
            Logger::Log(LOG_LEVEL_WARN, "failed to release data buffer");
        ReserveData(buff, len, data_off);
        Write6BInteger(edge_ptrs.offset_ptr, data_off);
        mm.WriteData(edge_ptrs.offset_ptr, OFFSET_SIZE, edge_ptrs.offset+EDGE_NODE_LEADING_POS);
    }
    else
    {
        uint8_t node_buff[NODE_EDGE_KEY_FIRST];
        size_t node_off = Get6BInteger(edge_ptrs.offset_ptr);

        if(mm.ReadData(node_buff, EDGE_NODE_LEADING_POS, node_off, true) != EDGE_NODE_LEADING_POS)
            return MBError::READ_ERROR;

        if(node_buff[0] & FLAG_NODE_MATCH)
        {
            inc_count = false;
            if(!overwrite)
                return MBError::IN_DICT;

            data_off = Get6BInteger(node_buff+2);
            if(ReleaseBuffer(data_off) != MBError::SUCCESS)
                Logger::Log(LOG_LEVEL_WARN, "failed to release data buffer");
        }
        else
        {
            // set the match flag
            node_buff[0] |= FLAG_NODE_MATCH;
        }

        ReserveData(buff, len, data_off);
        Write6BInteger(node_buff+2, data_off);
        mm.WriteData(node_buff, NODE_EDGE_KEY_FIRST, node_off);
    }

    return MBError::SUCCESS;
}

// delta should be either +1 or -1.
void Dict::UpdateNumReader(int delta) const
{
    header->num_reader += delta;
    if(header->num_reader < 0)
        header->num_reader = 0;

    Logger::Log(LOG_LEVEL_INFO, "number of reader is set to: %d",
                header->num_reader);
}

// delta should be either +1 or -1.
int Dict::UpdateNumWriter(int delta) const
{
    if(delta > 0)
    {
        // Only one writer allowed
        if(header->num_writer > 0)
            return MBError::WRITER_EXIST;

        header->num_writer++;
    }
    else if(delta < 0)
        header->num_writer = 0;

    Logger::Log(LOG_LEVEL_INFO, "number of writer is set to: %d",
                             header->num_writer);
    return MBError::SUCCESS;
}

FreeList* Dict::GetFreeList() const
{
    return free_lists;
}

DictMem* Dict::GetMM() const
{
    return (DictMem*) &mm;
}

IndexHeader* Dict::GetHeader() const
{
    return header;
}

const std::string& Dict::GetDBDir() const
{
    return mb_dir;
}

size_t Dict::GetStartDataOffset() const
{
    return DATA_BUFFER_ALIGNMENT;
}

void Dict::ResetSlidingWindow() const
{
    db_file->ResetSlidingWindow();
}

}