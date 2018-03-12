#include "consumer.h"
#include "xdebug.h"
#include "mario_item.h"
#include "version.h"
#include <iostream>


namespace mario {


#if defined(MARIO_MEMORY)
Consumer::Consumer(uint64_t initial_offset, Handler *h, char* pool,
        uint64_t pool_len)
    : h_(h),
    initial_offset_(initial_offset),
    last_record_offset_(0),
    end_of_buffer_offset_(kBlockSize),
    pool_(pool),
    origin_(pool),
    pool_len_(pool_len)
{

}

Consumer::~Consumer()
{

}

/*
 * inline void Consumer::CheckEnd(const uint64_t distance)
 * {
 *     if (pool_len_ < distance) {
 *         pool_len_ = kPoolSize;
 *         pool_ = origin_;
 *     }
 * 
 * }
 */

inline void Consumer::ConsumeData(const int64_t distance)
{
    // log_info("pool_len %ld distance %ld %ld", pool_len_, distance, pool_len_ - distance);
    if (pool_len_ < distance) {
        /*
         * jump to the next block
         */
        pool_ = origin_ + ((((pool_ - origin_ + 1) / kPoolSize) + 1) * kBlockSize) % kPoolSize;
        pool_len_ = kPoolSize - (pool_ - origin_);
    } else {
        pool_ += distance;
        pool_len_ -= distance;
    }
    if (pool_len_ <= 0) {
        // log_info("in the ifpool_len %ld distance %ld %ld", pool_len_, distance, pool_len_ - distance);
        pool_ = origin_;
        pool_len_ = kPoolSize;
    }
    // log_info("after pool %p pool_len %ld distance %ld", pool_, pool_len_, distance);
}

unsigned int Consumer::ReadMemoryRecord(Slice *result)
{
    Status s;
    if (end_of_buffer_offset_ - last_record_offset_ <= kHeaderSize) {
        ConsumeData(end_of_buffer_offset_ - last_record_offset_);
        last_record_offset_ = 0;
    }

    const char* header = pool_;
    const uint32_t a = static_cast<uint32_t>(header[0]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[1]) & 0xff;
    const uint32_t c = static_cast<uint32_t>(header[2]) & 0xff;
    const unsigned int type = header[3];
    const uint32_t length = a | (b << 8) | (c << 16);

    if (type == kZeroType || length == 0) {
        return kOldRecord;
    }

    *result = Slice(pool_ + kHeaderSize, length);
    ConsumeData(static_cast<uint64_t>(kHeaderSize) + static_cast<uint64_t>(length));
    last_record_offset_ += kHeaderSize + length;
    return type;
}

#endif

#if defined(MARIO_MMAP)
Consumer::Consumer(SequentialFile* const queue, Handler *h, Version* version,
        uint32_t filenum)
    : h_(h),
    initial_offset_(0),
    end_of_buffer_offset_(kBlockSize),
    queue_(queue),
    backing_store_(new char[kBlockSize]),
    buffer_(),
    eof_(false),
    version_(version),
    filenum_(filenum)
{
    // log_info("initial_offset %llu", initial_offset);
    last_record_offset_ = version_->con_offset() % kBlockSize;
    queue_->Skip(version_->con_offset());
    // log_info("status %s", s.ToString().c_str());
}

Consumer::~Consumer()
{
    delete [] backing_store_;
}

unsigned int Consumer::ReadPhysicalRecord(Slice *result)
{
    Status s;
    if (end_of_buffer_offset_ - last_record_offset_ <= kHeaderSize) {
        queue_->Skip(end_of_buffer_offset_ - last_record_offset_);
        version_->rise_con_offset(end_of_buffer_offset_ - last_record_offset_);
        version_->StableSave();
        last_record_offset_ = 0;
    }
    buffer_.clear();
    s = queue_->Read(kHeaderSize, &buffer_, backing_store_);
    if (s.IsEndFile()) {
        return kEof;
    } else if (!s.ok()) {
        return kBadRecord;
    }
    const char* header = buffer_.data();
    const uint32_t a = static_cast<uint32_t>(header[0]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[1]) & 0xff;
    const uint32_t c = static_cast<uint32_t>(header[2]) & 0xff;
    const unsigned int type = header[3];
    const uint32_t length = a | (b << 8) | (c << 16);
    if (type == kZeroType || length == 0) {
        buffer_.clear();
        return kOldRecord;
    }
    buffer_.clear();
    s = queue_->Read(length, &buffer_, backing_store_);
    *result = Slice(buffer_.data(), buffer_.size());
    last_record_offset_ += kHeaderSize + length;
    if (s.ok()) {
        version_->rise_con_offset(kHeaderSize + length);
        version_->StableSave();
    }
    return type;
}

#endif

Consumer::Handler::~Handler() {};

Status Consumer::Consume(std::string &scratch)
{
    Status s;
    // log_info("last_record_offset %llu initial_offset %llu", last_record_offset_, initial_offset_);
    if (last_record_offset_ < initial_offset_) {
        return Status::IOError("last_record_offset exceed");
    }

    Slice fragment;
    while (true) {
#if defined(MARIO_MEMORY)
        const unsigned int record_type = ReadMemoryRecord(&fragment);
#endif

#if defined(MARIO_MMAP)
        const unsigned int record_type = ReadPhysicalRecord(&fragment);
#endif
        switch (record_type) {
        case kFullType:
            scratch = std::string(fragment.data(), fragment.size());
            s = Status::OK();
            break;
        case kFirstType:
            scratch.assign(fragment.data(), fragment.size());
            s = Status::NotFound("Middle Status");
            break;
        case kMiddleType:
            scratch.append(fragment.data(), fragment.size());
            s = Status::NotFound("Middle Status");
            break;
        case kLastType:
            scratch.append(fragment.data(), fragment.size());
            s = Status::OK();
            break;
        case kEof:
            return Status::EndFile("Eof");
        case kBadRecord:
            return Status::Corruption("Data Corruption");
        case kOldRecord:
            queue_->Reverse(kHeaderSize);
            return Status::EndFile("Eof");
        default:
            s = Status::Corruption("Unknow reason");
            break;
        }
        // TODO:do handler here
        if (s.ok()) {
            break;
        }
    }
    return Status::OK();
}

}
