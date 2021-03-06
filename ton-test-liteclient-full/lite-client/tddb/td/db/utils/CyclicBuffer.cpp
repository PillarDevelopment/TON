#include "CyclicBuffer.h"

namespace td {
namespace detail {
class CyclicBuffer : public StreamWriterInterface, public StreamReaderInterface {
 public:
  using Options = ::td::CyclicBuffer::Options;

  CyclicBuffer(Options options) {
    shared_.options_ = options;
    shared_.raw_data_ = std::make_unique<char[]>(options.size() + options.alignment - 1);
    auto pos = reinterpret_cast<uint64>(shared_.raw_data_.get());
    auto offset = (options.alignment - static_cast<int64>(pos % options.alignment)) % options.alignment;
    CHECK(offset < options.alignment);
    shared_.data_ = MutableSlice(shared_.raw_data_.get() + offset, options.size());
  }

  // StreamReaderInterface
  size_t reader_size() override {
    auto offset = reader_.pos_.load(std::memory_order_relaxed);
    auto size = writer_.pos_.load(std::memory_order_acquire) - offset;
    return size;
  }
  Slice prepare_read() override {
    auto offset = reader_.pos_.load(std::memory_order_relaxed);
    auto size = writer_.pos_.load(std::memory_order_acquire) - offset;
    if (size == 0) {
      return {};
    }
    offset %= (shared_.options_.chunk_size * shared_.options_.count);
    return shared_.data_.substr(offset).truncate(size).truncate(shared_.options_.chunk_size);
  }
  Span<IoSlice> prepare_readv() override {
    reader_.io_slice_ = as_io_slice(prepare_read());
    return Span<IoSlice>(&reader_.io_slice_, 1);
  }
  void confirm_read(size_t size) override {
    reader_.pos_.store(reader_.pos_.load(std::memory_order_relaxed) + size);
  }

  void close_reader(Status error) override {
    CHECK(!reader_.is_closed_);
    reader_.status_ = std::move(error);
    reader_.is_closed_.store(true, std::memory_order_release);
  }
  bool is_writer_closed() const override {
    return writer_.is_closed_.load(std::memory_order_acquire);
  }
  Status &writer_status() override {
    CHECK(is_writer_closed());
    return writer_.status_;
  }

  // StreamWriterInterface
  size_t writer_size() override {
    auto offset = reader_.pos_.load(std::memory_order_acquire);
    auto size = writer_.pos_.load(std::memory_order_relaxed) - offset;
    return size;
  }
  MutableSlice prepare_write() override {
    auto max_offset =
        reader_.pos_.load(std::memory_order_acquire) + shared_.options_.chunk_size * (shared_.options_.count - 1);
    auto offset = writer_.pos_.load(std::memory_order_relaxed);
    if (offset > max_offset) {
      return {};
    }
    offset %= (shared_.options_.chunk_size * shared_.options_.count);
    return shared_.data_.substr(offset, shared_.options_.chunk_size);
  }
  MutableSlice prepare_write_at_least(size_t size) override {
    UNREACHABLE();
  }
  void confirm_write(size_t size) override {
    writer_.pos_.store(writer_.pos_.load(std::memory_order_relaxed) + size);
  }
  void append(Slice data) override {
    UNREACHABLE();
  }
  void append(BufferSlice data) override {
    UNREACHABLE();
  }
  void append(std::string data) override {
    UNREACHABLE();
  }
  void close_writer(Status error) override {
    CHECK(!writer_.is_closed_);
    writer_.status_ = std::move(error);
    writer_.is_closed_.store(true, std::memory_order_release);
  }
  bool is_reader_closed() const override {
    return reader_.is_closed_.load(std::memory_order_acquire);
  }
  Status &reader_status() override {
    CHECK(is_reader_closed());
    return reader_.status_;
  }

 private:
  struct SharedData {
    std::unique_ptr<char[]> raw_data_;
    MutableSlice data_;
    Options options_;
  } shared_;

  struct ReaderData {
    std::atomic<int64> pos_{0};
    std::atomic<bool> is_closed_{false};
    Status status_;
    IoSlice io_slice_;
  } reader_;

  char pad[128];

  struct WriterData {
    std::atomic<int64> pos_{0};
    std::atomic<bool> is_closed_{false};
    Status status_;
  } writer_;
};
}  // namespace detail

std::pair<CyclicBuffer::Reader, CyclicBuffer::Writer> CyclicBuffer::create(Options options) {
  auto impl = std::make_shared<detail::CyclicBuffer>(options);
  return {Reader(impl), Writer(impl)};
}
}  // namespace td
