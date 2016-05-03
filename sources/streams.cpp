#include "streams.h"

#include <algorithm>
#include <array>
#include <assert.h>
#include <memory>
#include <string.h>
#include <utility>

#include <nettle/hmac.h>

namespace securefs
{
namespace internal
{
    class InvalidHMACStreamException : public InvalidFormatException
    {
    private:
        id_type m_id;
        std::string m_msg;

    public:
        explicit InvalidHMACStreamException(const id_type& id, std::string msg)
        {
            memcpy(m_id.data(), id.data(), id.size());
            m_msg.swap(msg);
        }

        const char* type_name() const noexcept override { return "InvalidHMACStreamException"; }
        std::string message() const override { return m_msg; }
    };

    class HMACStream final : public StreamBase
    {
    private:
        key_type m_key;
        id_type m_id;
        hmac_sha256_ctx m_ctx;
        std::shared_ptr<StreamBase> m_stream;
        bool is_dirty;

        static const size_t hmac_length = SHA256_DIGEST_SIZE;

    private:
        const id_type& id() const noexcept { return m_id; }
        const key_type& key() const noexcept { return m_key; }

        void run_mac()
        {
            nettle_hmac_sha256_update(&m_ctx, m_id.size(), m_id.data());
            std::array<byte, 2000> buffer;
            offset_type off = hmac_length;
            while (true)
            {
                auto rc = m_stream->read(buffer.data(), off, buffer.size());
                if (rc == 0)
                    break;
                nettle_hmac_sha256_update(&m_ctx, rc, buffer.data());
                off += rc;
            }
        }

    public:
        explicit HMACStream(const key_type& key_,
                            const id_type& id_,
                            std::shared_ptr<StreamBase> stream,
                            bool check = true)
            : m_key(key_), m_id(id_), m_stream(std::move(stream)), is_dirty(false)
        {
            if (!m_stream)
                NULL_EXCEPT();
            nettle_hmac_sha256_set_key(&m_ctx, m_key.size(), m_key.data());

            if (check)
            {
                std::array<byte, hmac_length> hmac;
                auto rc = m_stream->read(hmac.data(), 0, hmac.size());
                if (rc == 0)
                    return;
                if (rc != hmac_length)
                    throw InvalidHMACStreamException(
                        id(), "The header field for stream is not of enough length");

                run_mac();
                std::array<byte, hmac_length> calculated_mac;
                nettle_hmac_sha256_digest(&m_ctx, calculated_mac.size(), calculated_mac.data());
                if (securefs::constant_time_compare(
                        calculated_mac.data(), hmac.data(), calculated_mac.size(), hmac.size())
                    != 0)
                    throw InvalidHMACStreamException(m_id, "Invalid HMAC");
            }
        }

        ~HMACStream()
        {
            try
            {
                flush();
            }
            catch (...)
            {
                // ignore
            }
        }

        void flush() override
        {
            if (!is_dirty)
                return;
            run_mac();
            std::array<byte, hmac_length> hmac;
            nettle_hmac_sha256_digest(&m_ctx, hmac.size(), hmac.data());
            m_stream->write(hmac.data(), 0, hmac.size());
            m_stream->flush();
            is_dirty = false;
        }

        length_type size() const override
        {
            auto sz = m_stream->size();
            if (sz < hmac_length)
                return 0;
            return sz - hmac_length;
        }

        length_type read(void* output, offset_type off, length_type len) override
        {
            return m_stream->read(output, off + hmac_length, len);
        }

        void write(const void* input, offset_type off, length_type len) override
        {
            m_stream->write(input, off + hmac_length, len);
            is_dirty = true;
        }

        void resize(length_type len) override
        {
            m_stream->resize(len + hmac_length);
            is_dirty = true;
        }

        bool is_sparse() const noexcept override { return m_stream->is_sparse(); }
    };
}

std::shared_ptr<StreamBase> make_stream_hmac(const key_type& key_,
                                             const id_type& id_,
                                             std::shared_ptr<StreamBase> stream,
                                             bool check)
{
    return std::make_shared<internal::HMACStream>(key_, id_, std::move(stream), check);
}

length_type CryptStream::read_block(offset_type block_number, void* output)
{
    auto rc = m_stream->read(output, block_number * m_block_size, m_block_size);
    if (rc == 0)
        return 0;
    decrypt(block_number, output, output, rc);
    return rc;
}

length_type
CryptStream::read_block(offset_type block_number, void* output, offset_type begin, offset_type end)
{
    assert(begin <= m_block_size && end <= m_block_size);

    if (begin == 0 && end == m_block_size)
        return read_block(block_number, output);

    if (begin >= end)
        return 0;

    securefs::SecureByteBlock buffer(m_block_size);
    auto rc = read_block(block_number, buffer.data());
    if (rc <= begin)
        return 0;
    end = std::min<offset_type>(end, rc);
    memcpy(output, buffer.data() + begin, end - begin);
    return end - begin;
}

void CryptStream::write_block(offset_type block_number, const void* input, length_type length)
{
    assert(length <= m_block_size);
    std::unique_ptr<byte[]> buffer(
        new byte[length]);    // Ciphertext needs not be cleared after use
    encrypt(block_number, input, buffer.get(), length);
    m_stream->write(buffer.get(), block_number * m_block_size, length);
}

void CryptStream::read_then_write_block(offset_type block_number,
                                        const void* input,
                                        offset_type begin,
                                        offset_type end)
{
    assert(begin <= m_block_size && end <= m_block_size);

    if (begin == 0 && end == m_block_size)
        return write_block(block_number, input, m_block_size);
    if (begin >= end)
        return;

    securefs::SecureByteBlock buffer(m_block_size);
    auto rc = read_block(block_number, buffer.data());
    memcpy(buffer.data() + begin, input, end - begin);
    write_block(block_number, buffer.data(), std::max<length_type>(rc, end));
}

length_type CryptStream::read(void* output, offset_type offset, length_type length)
{
    length_type total = 0;

    while (length > 0)
    {
        auto block_num = offset / m_block_size;
        auto start_of_block = block_num * m_block_size;
        auto begin = offset - start_of_block;
        auto end = std::min<offset_type>(m_block_size, offset + length - start_of_block);
        auto rc = read_block(block_num, output, begin, end);
        total += rc;
        if (rc < end - begin)
            return total;
        output = static_cast<byte*>(output) + rc;
        offset += rc;
        length -= rc;
    }

    return total;
}

void CryptStream::write(const void* input, offset_type offset, length_type length)
{
    auto current_size = this->size();
    if (offset > current_size)
        resize(offset);

    unchecked_write(input, offset, length);
}

void CryptStream::unchecked_write(const void* input, offset_type offset, length_type length)
{
    while (length > 0)
    {
        auto block_num = offset / m_block_size;
        auto start_of_block = block_num * m_block_size;
        auto begin = offset - start_of_block;
        auto end = std::min<offset_type>(m_block_size, offset + length - start_of_block);
        read_then_write_block(block_num, input, begin, end);
        auto rc = end - begin;
        input = static_cast<const byte*>(input) + rc;
        offset += rc;
        length -= rc;
    }
}

void CryptStream::zero_fill(offset_type offset, offset_type finish)
{
    std::unique_ptr<byte[]> zeros(new byte[m_block_size]);
    memset(zeros.get(), 0, m_block_size);
    while (offset < finish)
    {
        auto block_num = offset / m_block_size;
        auto start_of_block = block_num * m_block_size;
        auto begin = offset - start_of_block;
        auto end = std::min<offset_type>(m_block_size, finish - start_of_block);
        read_then_write_block(block_num, zeros.get(), begin, end);
        auto rc = end - begin;
        offset += rc;
    }
}

void CryptStream::resize(length_type new_size)
{
    auto current_size = this->size();
    if (new_size == current_size)
        return;
    else if (new_size < current_size)
    {
        auto residue = new_size % m_block_size;
        auto block_num = new_size / m_block_size;
        if (residue > 0)
        {
            securefs::SecureByteBlock buffer(m_block_size);
            memset(buffer.data(), 0, buffer.size());
            (void)read_block(block_num, buffer.data());
            write_block(block_num, buffer.data(), residue);
        }
    }
    else
    {
        auto old_block_num = current_size / m_block_size;
        auto new_block_num = new_size / m_block_size;
        if (!is_sparse() || old_block_num == new_block_num)
            zero_fill(current_size, new_size);
        else
        {
            zero_fill(current_size, old_block_num * m_block_size + m_block_size);
            // No need to encrypt zeros in the middle
            zero_fill(new_block_num * m_block_size, new_size);
        }
    }
    m_stream->resize(new_size);
}

namespace internal
{
    class AESGCMCryptStream final : public CryptStream, public HeaderBase
    {
    public:
        unsigned get_iv_size() const noexcept { return m_iv_size; }

        unsigned get_mac_size() const noexcept { return 16; }

        unsigned get_meta_size() const noexcept { return get_iv_size() + get_mac_size(); }

        unsigned get_header_size() const noexcept { return 32; }

        unsigned get_encrypted_header_size() const noexcept
        {
            return get_header_size() + get_iv_size() + get_mac_size();
        }

        static const int64_t max_block_number = 1 << 30;

    private:
        HMACStream m_metastream;
        key_type m_key;
        id_type m_id;
        unsigned m_iv_size;
        bool m_check;

    private:
        length_type meta_position_for_iv(offset_type block_num) const noexcept
        {
            return get_encrypted_header_size() + get_meta_size() * (block_num);
        }

        void check_block_number(offset_type block_number)
        {
            if (block_number > max_block_number)
                throw StreamTooLongException(max_block_number * this->m_block_size,
                                             block_number * this->m_block_size);
        }

        const id_type& id() const noexcept { return m_id; }
        const key_type& key() const noexcept { return m_key; }

    public:
        explicit AESGCMCryptStream(std::shared_ptr<StreamBase> data_stream,
                                   std::shared_ptr<StreamBase> meta_stream,
                                   const key_type& data_key,
                                   const key_type& meta_key,
                                   const id_type& id_,
                                   bool check,
                                   unsigned block_size,
                                   unsigned iv_size)
            : CryptStream(data_stream, block_size)
            , m_metastream(meta_key, id_, meta_stream, check)
            , m_key(data_key)
            , m_id(id_)
            , m_iv_size(iv_size)
            , m_check(check)
        {
        }

    protected:
        void encrypt(offset_type block_number,
                     const void* input,
                     void* output,
                     length_type length) override
        {
            if (length == 0)
                return;
            check_block_number(block_number);

            auto buffer = make_unique_array<byte>(get_meta_size());
            byte* iv = buffer.get();
            byte* mac = iv + get_iv_size();

            do
            {
                generate_random(iv, get_iv_size());
            } while (is_all_zeros(iv, get_iv_size()));    // Null IVs are markers for sparse blocks
            aes_gcm_encrypt(input,
                            length,
                            id().data(),
                            id().size(),
                            key().data(),
                            key().size(),
                            iv,
                            get_iv_size(),
                            mac,
                            get_mac_size(),
                            output);
            auto pos = meta_position_for_iv(block_number);
            m_metastream.write(buffer.get(), pos, get_meta_size());
        }

        void decrypt(offset_type block_number,
                     const void* input,
                     void* output,
                     length_type length) override
        {
            if (length == 0)
                return;
            check_block_number(block_number);

            auto buffer = make_unique_array<byte>(get_meta_size());
            auto pos = meta_position_for_iv(block_number);
            if (m_metastream.read(buffer.get(), pos, get_meta_size()) != get_meta_size())
                throw CorruptedMetaDataException(id(), "MAC/IV not found");

            const byte* iv = buffer.get();
            byte* mac = buffer.get() + get_iv_size();

            if (is_all_zeros(iv, get_iv_size()))
            {
                memset(output, 0, length);
                return;
            }
            bool success = aes_gcm_decrypt(input,
                                           length,
                                           id().data(),
                                           id().size(),
                                           key().data(),
                                           key().size(),
                                           iv,
                                           get_iv_size(),
                                           mac,
                                           get_mac_size(),
                                           output);
            if (m_check && !success)
                throw MessageVerificationException(id(), block_number * m_block_size);
        }

    public:
        bool is_sparse() const noexcept override
        {
            return m_stream->is_sparse() && m_metastream.is_sparse();
        }

        void resize(length_type new_size) override
        {
            CryptStream::resize(new_size);
            auto num_blocks = (new_size + m_block_size - 1) / m_block_size;
            m_metastream.resize(meta_position_for_iv(num_blocks));
        }

        void flush() override
        {
            CryptStream::flush();
            m_metastream.flush();
        }

    private:
        length_type unchecked_read_header(void* output)
        {
            auto buffer = make_unique_array<byte>(get_encrypted_header_size());
            auto rc = m_metastream.read(buffer.get(), 0, get_encrypted_header_size());
            if (rc == 0)
                return 0;
            if (rc != get_encrypted_header_size())
                throw CorruptedMetaDataException(id(), "Not enough header field");

            byte* iv = buffer.get();
            byte* mac = iv + get_iv_size();
            byte* ciphertext = mac + get_mac_size();
            aes_gcm_decrypt(ciphertext,
                            get_header_size(),
                            id().data(),
                            id().size(),
                            key().data(),
                            key().size(),
                            iv,
                            get_iv_size(),
                            mac,
                            get_mac_size(),
                            output);
            return get_encrypted_header_size();
        }

        void unchecked_write_header(const void* input)
        {
            auto buffer = make_unique_array<byte>(get_encrypted_header_size());
            byte* iv = buffer.get();
            byte* mac = iv + get_iv_size();
            byte* ciphertext = mac + get_mac_size();
            generate_random(iv, get_iv_size());

            aes_gcm_encrypt(input,
                            get_header_size(),
                            id().data(),
                            id().size(),
                            key().data(),
                            key().size(),
                            iv,
                            get_iv_size(),
                            mac,
                            get_mac_size(),
                            ciphertext);
            m_metastream.write(buffer.get(), 0, get_encrypted_header_size());
        }

    public:
        bool read_header(void* output, length_type length) override
        {
            if (length > get_header_size())
                throw InvalidArgumentException("Header too long");
            if (length == get_header_size())
                return unchecked_read_header(output);

            securefs::SecureByteBlock buffer(get_header_size());
            auto rc = unchecked_read_header(buffer.data());
            memcpy(output, buffer.data(), std::min(length, rc));
            return rc != 0;
        }

        length_type max_header_length() const noexcept override { return get_header_size(); }

        void write_header(const void* input, length_type length) override
        {
            if (length > get_header_size())
                throw InvalidArgumentException("Header too long");

            if (length == get_header_size())
                return unchecked_write_header(input);

            securefs::SecureByteBlock buffer(get_header_size());
            memcpy(buffer.data(), input, length);
            memset(buffer.data() + length, 0, buffer.size() - length);
            unchecked_write_header(buffer.data());
        }

        void flush_header() override { m_metastream.flush(); }
    };
}

std::pair<std::shared_ptr<CryptStream>, std::shared_ptr<HeaderBase>>
make_cryptstream_aes_gcm(std::shared_ptr<StreamBase> data_stream,
                         std::shared_ptr<StreamBase> meta_stream,
                         const key_type& data_key,
                         const key_type& meta_key,
                         const id_type& id_,
                         bool check,
                         unsigned block_size,
                         unsigned iv_size)
{
    auto stream = std::make_shared<internal::AESGCMCryptStream>(std::move(data_stream),
                                                                std::move(meta_stream),
                                                                data_key,
                                                                meta_key,
                                                                id_,
                                                                check,
                                                                block_size,
                                                                iv_size);
    return {stream, stream};
}
}
