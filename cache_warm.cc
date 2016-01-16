// -------------------------------------------------------------------
//
// cache_warm.cc
//
// Copyright (c) 2011-2016 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#include <string>

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/cache.h"
#include "util/cache2.h"

namespace leveldb {


/**
 * Encode (append) the information from TableAndFile object to
 * destination screen.  Writes only values needed for opening
 * a table file.
 */
static void
EncodeFileCacheObject(
    std::string & Dest,
    TableAndFile & Table)
{
    PutVarint32(&Dest, VersionEdit::kFileCacheObject);
    PutVarint32(&Dest, Table.level);
    PutVarint64(&Dest, Table.file_number);
    PutVarint64(&Dest, Table.table->GetFileSize());

    return;

}   // EncodeFileCacheObject




class WarmingAccumulator : public CacheAccumulator
{
protected:
    size_t m_ValueCount;
    std::string m_Record;

public:
    WarmingAccumulator()
    {
        m_ValueCount=0;
        m_Record.reserve(4096);
    };

    std::string & GetRecord() {return(m_Record);};

    size_t GetCount() const {return(m_ValueCount);};

    virtual bool operator()(void * Value)
    {
        if (NULL!=Value)
        {
            TableAndFile * tf;

            tf=(TableAndFile *)Value;
            EncodeFileCacheObject(m_Record, *tf);
            ++m_ValueCount;
        }   // if
        return(true);
    };

};  // class WarmingAccumulator


/**
 * Riak specific routine to push list of open files to disk
 */
void
TableCache::SaveOpenFileList()
{
    Status s;
    std::string cow_name;
    WritableFile * cow_file;
    log::Writer * cow_log;

    cow_name=CowFileName(dbname_);
    s = env_->NewWritableFile(cow_name, &cow_file, 4*1024L);
    if (s.ok())
    {
        WarmingAccumulator acc;

        cow_log=new log::Writer(cow_file);
        doublecache_.GetFileCache()->WalkCache(acc);
        s = cow_log->AddRecord(acc.GetRecord());
        delete cow_log;
        delete cow_file;

        if (s.ok())
        {
            Log(options_->info_log, "Wrote %zd file cache objects for warming.",
                acc.GetCount());
        }   // if
        else
        {
            Log(options_->info_log, "Error writing cache object file %s (error %s)",
                cow_name.c_str(), s.ToString().c_str());
            env_->DeleteFile(cow_name);
        }   // else
    }   // if
    else
    {
        Log(options_->info_log, "Unable to create cache object file %s (error %s)",
            cow_name.c_str(), s.ToString().c_str());
    }   // else

    return;

}   // TableCache::SaveOpenFiles


/**
 * Riak specific routine to read list of previously open files
 *  and preload them into the table cache
 */
void
TableCache::PreloadTableCache()
{
    struct LogReporter : public log::Reader::Reporter {
        Env* env;
        Logger* info_log;
        const char* fname;
        Status* status;
        virtual void Corruption(size_t bytes, const Status& s) {
            Log(info_log, "%s%s: dropping %d bytes; %s",
                (this->status == NULL ? "(ignoring error) " : ""),
                fname, static_cast<int>(bytes), s.ToString().c_str());
            if (this->status != NULL && this->status->ok()) *this->status = s;
        }
    };

    Status s;
    std::string cow_name;
    SequentialFile * cow_file;
    log::Reader * cow_log;
    size_t obj_count(0);

    cow_name=CowFileName(dbname_);
    s = env_->NewSequentialFile(cow_name, &cow_file);
    if (s.ok())
    {
        // Create the log reader.
        LogReporter reporter;
        reporter.env = env_;
        reporter.info_log = options_->info_log;
        reporter.fname = cow_name.c_str();
        reporter.status = &s;

        std::string buffer;
        Slice record;

        cow_log=new log::Reader(cow_file, &reporter, true, 0);

        while (cow_log->ReadRecord(&record, &buffer) && s.ok())
        {
            Slice input = record;
            uint32_t tag, level;
            uint64_t file_no, file_size;
            Cache::Handle * handle;
            Status s2;

            // the on disk format is created in WriteFileCacheObjectWarming()
            //  (util/cache2.cc)
            while (GetVarint32(&input, &tag))
            {
                if (VersionEdit::kFileCacheObject==tag)
                {
                  GetVarint32(&input, &level);
                  GetVarint64(&input, &file_no);
                  GetVarint64(&input, &file_size);

                  // do not care if this succeeds, but need status
                  //  for handle maintenance
                  handle=NULL;

                  // set compaction flag to suggest Linux start pre-reading the files
                  s2=FindTable(file_no, file_size, level, &handle, (level<config::kNumOverlapLevels));

                  if (s2.ok())
                  {
                      cache_->Release(handle);
                      handle=NULL;
                      ++obj_count;
                  }   // if
                }   // if
                else
                {
                    Log(options_->info_log,"Unknown tag (%u) seen in preload file %s",
                        tag, cow_name.c_str());
                }   // else
            }   // while
      }   // while

      delete cow_log;
      delete cow_file;

      // delete the physical file at this point
      //   (keep bad file for possible review?)
      env_->DeleteFile(cow_name);

      Log(options_->info_log, "File cache warmed with %zd files.", obj_count);
  }   // if
  else
  {
      Log(options_->info_log, "No cache warming file detected.");
  }   // else

}   // TableCache::PreloadTableCache

}  // namespace leveldb