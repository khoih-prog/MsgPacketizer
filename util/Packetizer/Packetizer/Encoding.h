#pragma once
#ifndef HT_SERIAL_PACKETIZER_ENCODING_H
#define HT_SERIAL_PACKETIZER_ENCODING_H

#include "Types.h"
#include "util/CRCx/CRCx.h"
#include "util/ArxTypeTraits/ArxTypeTraits.h"

namespace ht {
namespace serial {
namespace packetizer {

namespace encoding
{

    class EncoderBase
    {
    protected:
        Packet buffer;
    public:
        virtual ~EncoderBase() {}
        virtual void header() = 0;
        virtual void append(const uint8_t data) = 0;
        virtual void footer() = 0;
        const uint8_t* data() const { return buffer.data(); };
        size_t size() const { return buffer.size(); };
        const Packet& packet() const { return buffer; }

        size_t encode(const uint8_t index, const uint8_t* src, const size_t size, bool b_crc = default_option.b_crc)
        {
            header();
            append(index);
            for (size_t i = 0; i < size; ++i) append(src[i]);
            if (b_crc) append(crcx::crc8(src, size)); // crc is only for *src (exclude index)
            footer();
            return this->size();
        }

        size_t encode(const uint8_t* src, const size_t size, bool b_crc = default_option.b_crc)
        {
            header();
            for (size_t i = 0; i < size; ++i) append(src[i]);
            if (b_crc) append(crcx::crc8(src, size)); // crc is only for *src (exclude index)
            footer();
            return this->size();
        }
    };


    class DecoderBase
    {
    protected:

        Packet buffer;
        bool b_index {default_option.b_index};
        bool b_crc {default_option.b_crc};
        bool b_parsing {false};
        uint32_t err_count {0};

    public:

        virtual ~DecoderBase() {}

        virtual void feed(const uint8_t data, PacketQueue& packets, IndexQueue& indices)
        {
            if (data == marker())
            {
                if (!buffer.empty())
                {
                    buffer.emplace_back(data);
                    Packet packet = decode(buffer.data(), buffer.size());
                    if (b_index)
                    {
                        indices.emplace_back(packet.front());
                        packet.erase(packet.begin()); // index
                    }
                    else
                    {
                        indices.emplace_back(0);
                    }
                    if (b_crc)
                    {
                        if (crcx::crc8(packet.data(), packet.size() - 1) == packet.back())
                        {
                            packet.pop_back(); // crc
                            packets.emplace_back(std::move(packet));
                        }
                        else
                        {
                            ++err_count;
                        }
                    }
                    else
                    {
                        packets.emplace_back(std::move(packet));
                    }
                }
                reset();
            }
            else
            {
                buffer.emplace_back(data);
                b_parsing = true;
            }
        }

        void reset()
        {
            buffer.clear();
            b_parsing = false;
        }

        bool parsing() const { return b_parsing; }
        uint32_t errors() const { return err_count; }

        bool indexing() const { return b_index; }
        bool verifying() const { return b_crc; }

        void indexing(bool b)  { b_index = b; }
        void verifying(bool b)  { b_crc = b; }

        virtual Packet decode(const uint8_t* src, const size_t size) = 0;
        virtual uint8_t marker() const = 0;
    };

    using EncoderBaseRef = shared_ptr<EncoderBase>;
    using DecoderBaseRef = shared_ptr<DecoderBase>;


    namespace cobs
    {
        const uint8_t MARKER_END {0};
        const uint8_t MARKER_DUMMY {0};
        const uint8_t NEXT_ZERO_AFTER_NOZERO_PACKET {0};

        class Encoder : public EncoderBase
        {
            uint8_t next_zero_count {1};
            size_t next_zero_index {0};

        public:

            virtual ~Encoder() {}

            static EncoderBaseRef create() { return make_shared<Encoder>(); }

            virtual void header() override
            {
                next_zero_count = 1;
                next_zero_index = 0;
                buffer.clear();
                buffer.emplace_back(MARKER_DUMMY); // for next zero index
            }

            virtual void append(const uint8_t data) override
            {
                if (next_zero_count == NEXT_ZERO_AFTER_NOZERO_PACKET)
                {
                    // after completed no-zero 254 byte block, continue to next block
                    next_zero_count = 1;
                    next_zero_index = buffer.size();
                    buffer.emplace_back(MARKER_DUMMY); // dummy for next zero index
                }

                if (data != MARKER_END)
                {
                    buffer.emplace_back(data);
                    ++next_zero_count;
                }
                else // input is zero
                {
                    buffer[next_zero_index] = next_zero_count;
                    next_zero_count = 1;
                    next_zero_index = buffer.size();
                    buffer.emplace_back(MARKER_DUMMY); // for next zero index
                }

                if (next_zero_count == 0xFF) // complete no-zero 254 byte block
                {
                    buffer[next_zero_index] = next_zero_count;
                    next_zero_count = NEXT_ZERO_AFTER_NOZERO_PACKET;
                    next_zero_index = buffer.size();
                }
            }

            virtual void footer() override
            {
                buffer[next_zero_index] = next_zero_count; // last zero before end marker
                buffer.emplace_back(MARKER_END);
            }
        };


        class Decoder : public DecoderBase
        {
        public:

            virtual ~Decoder() {}

            static DecoderBaseRef create() { return make_shared<Decoder>(); }

        private:

            virtual Packet decode(const uint8_t* src, const size_t size) override
            {
                Packet packet;
                const uint8_t* const end = src + size;
                uint8_t next_zero = 0xFF;
                uint8_t rest = 0;

                for (; src < end; ++src, --rest)
                {
                    if (rest == 0) // overhead byte or zero byte
                    {
                        if (next_zero != 0xFF) // zero byte
                        {
                            rest = next_zero = *src; // size to next zero byte
                            if (next_zero == MARKER_END)
                                break; // end packet
                            packet.emplace_back(0);
                        }
                        else // overhead byte of no-zero packet
                        {
                            rest = next_zero = *src; // size to next zero byte
                            if (next_zero == MARKER_END)
                                break; // end packet
                        }
                    }
                    else
                    {
                        packet.emplace_back(*src);
                    }
                }

                return std::move(packet);
            }

            virtual uint8_t marker() const { return MARKER_END; }
        };

    } // namespace cobs


    namespace slip
    {
        static constexpr uint8_t MARKER_END {0xC0};
        static constexpr uint8_t MARKER_ESC {0xDB};
        static constexpr uint8_t MARKER_ESC_END {0xDC};
        static constexpr uint8_t MARKER_ESC_ESC {0xDD};

        class Encoder : public EncoderBase
        {
        public:

            virtual ~Encoder() {}

            static EncoderBaseRef create() { return make_shared<Encoder>(); }

            virtual void header() override
            {
                buffer.clear();
                buffer.emplace_back(MARKER_END); // double-ended slip
            }

            virtual void append(const uint8_t data) override
            {
                if(data == MARKER_END)
                {
                    buffer.emplace_back(MARKER_ESC);
                    buffer.emplace_back(MARKER_ESC_END);
                }
                else if(data == MARKER_ESC)
                {
                    buffer.emplace_back(MARKER_ESC);
                    buffer.emplace_back(MARKER_ESC_ESC);
                }
                else
                {
                    buffer.emplace_back(data);
                }
            }

            virtual void footer() override
            {
                buffer.emplace_back(MARKER_END);
            }
        };

        class Decoder : public DecoderBase
        {
        public:

            virtual ~Decoder() {}

            static DecoderBaseRef create() { return make_shared<Decoder>(); }

        private:

            virtual Packet decode(const uint8_t* src, const size_t size) override
            {
                Packet packet;

                for (size_t i = 0; i < size; ++i)
                {
                    if (src[i] == MARKER_END) // last byte or double-ended slip
                    {
                    }
                    else if (src[i] == MARKER_ESC) // escaped byte
                    {
                        if (src[i + 1] == MARKER_ESC_END)
                            packet.emplace_back(MARKER_END);
                        else if (src[i + 1] == MARKER_ESC_ESC)
                            packet.emplace_back(MARKER_ESC);
                        ++i;
                    }
                    else
                    {
                        packet.emplace_back(src[i]);
                    }
                }

                return std::move(packet);
            }

            virtual uint8_t marker() const { return MARKER_END; }
        };

    } // namspace slip

} // namespace encoding

using EncoderBaseRef = encoding::EncoderBaseRef;
using DecoderBaseRef = encoding::DecoderBaseRef;


template <typename Encoding>
using encoder_t = typename std::conditional<
    std::is_same<Encoding, encoding::COBS>::value,
    encoding::cobs::Encoder,
    encoding::slip::Encoder
>::type;

template <typename Encoding>
using decoder_t = typename std::conditional<
    std::is_same<Encoding, encoding::COBS>::value,
    encoding::cobs::Decoder,
    encoding::slip::Decoder
>::type;


template <typename Encoding>
inline auto create()
-> typename std::enable_if <
    std::is_same<Encoding, encoding::cobs::Encoder>::value ||
    std::is_same<Encoding, encoding::slip::Encoder>::value,
    EncoderBaseRef
>::type
{
    return Encoding::create();
}

template <typename Encoding>
inline auto create()
-> typename std::enable_if <
    std::is_same<Encoding, encoding::cobs::Decoder>::value ||
    std::is_same<Encoding, encoding::slip::Decoder>::value,
    DecoderBaseRef
>::type
{
    return Encoding::create();
}

} // packetizer
} // serial
} // ht

#endif // HT_SERIAL_PACKETIZER_ENCODING_H
