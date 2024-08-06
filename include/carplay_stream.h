#ifndef CARPLAY_STREAM_H_
#define CARPLAY_STREAM_H_

#include <cstdint>

#include <QIODevice>


class CarPlayStream : public QIODevice
{
    // https://www.asawicki.info/news_1468_circular_buffer_of_raw_binary_data_in_c
    Q_OBJECT

  public:
    CarPlayStream();
    ~CarPlayStream();

    qint64 populate(const char *data, qint64 maxSize);

    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

    qint64 bytesAvailable() const override;

  private:
    size_t beg_index_, end_index_, size_, capacity_;
    char *data_;
};

#endif  // CARPLAY_STREAM_H_
