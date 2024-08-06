#include "carplay_stream.h"

#include <spdlog/spdlog.h>


CarPlayStream::CarPlayStream()
    : QIODevice()
    , beg_index_(0)
    , end_index_(0)
    , size_(0)
    , capacity_(512u * 1024u)
{
    data_ = new char[capacity_];

    open(QIODeviceBase::ReadOnly);
}

CarPlayStream::~CarPlayStream()
{
    delete [] data_;
}

qint64 CarPlayStream::readData(char *data, qint64 maxSize)
{
    if (maxSize == 0) return 0;

    size_t capacity = capacity_;
    size_t bytes_to_read = std::min(maxSize, static_cast<qint64>(size_));

    // Read in a single step
    if (bytes_to_read <= capacity - beg_index_)
    {
        memcpy(data, data_ + beg_index_, bytes_to_read);
        beg_index_ += bytes_to_read;
        if (beg_index_ == capacity) beg_index_ = 0;
    }
    // Read in two steps
    else
    {
        size_t size_1 = capacity - beg_index_;
        memcpy(data, data_ + beg_index_, size_1);
        size_t size_2 = bytes_to_read - size_1;
        memcpy(data + size_1, data_, size_2);
        beg_index_ = size_2;
    }

    size_ -= bytes_to_read;
    SPDLOG_INFO("CarPlayStream::readData, maxSize = {}, bytes_to_read = {}", maxSize, bytes_to_read);
    return bytes_to_read;
}

qint64 CarPlayStream::writeData(const char *data, qint64 maxSize)
{
    return -1;
}


qint64 CarPlayStream::populate(const char *data, qint64 maxSize)
{
    SPDLOG_INFO("CarPlayStream::populate, maxSize = {}", maxSize);
    if (maxSize == 0) return 0;

    size_t capacity = capacity_;
    size_t bytes_to_write = std::min(maxSize, static_cast<qint64>(capacity - size_));

    // Write in a single step
    if (bytes_to_write <= capacity - end_index_)
    {
        memcpy(data_ + end_index_, data, bytes_to_write);
        end_index_ += bytes_to_write;
        if (end_index_ == capacity) end_index_ = 0;
    }
    // Write in two steps
    else
    {
        size_t size_1 = capacity - end_index_;
        memcpy(data_ + end_index_, data, size_1);
        size_t size_2 = bytes_to_write - size_1;
        memcpy(data_, data + size_1, size_2);
        end_index_ = size_2;
    }

    size_ += bytes_to_write;

    emit(readyRead());


    return bytes_to_write;
}

qint64 CarPlayStream::bytesAvailable() const
{
    return size_;
}



#include "moc_carplay_stream.cpp"