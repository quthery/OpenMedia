#include <cstring>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <map>
#include <queue>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <span>

// libwebm headers
#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

namespace openmedia {

// ---------------------------------------------------------------------------
// IMkvReader adapter — bridges openmedia::InputStream → libwebm's IMkvReader
// ---------------------------------------------------------------------------
class InputStreamMkvReader final : public mkvparser::IMkvReader {
public:
    explicit InputStreamMkvReader(InputStream* stream) : stream_(stream) {}

    // Read `length` bytes at `position` into `buffer`.
    int Read(long long position, long length, unsigned char* buffer) override {
        if (!stream_ || position < 0 || length < 0) return -1;
        if (!stream_->seek(static_cast<int64_t>(position), Whence::BEG)) return -1;

        const size_t read = stream_->read(std::span<uint8_t>(buffer, static_cast<size_t>(length)));
        return (read == static_cast<size_t>(length)) ? 0 : -1;
    }

    // Return the total file length in `total`; -1 if unknown.
    int Length(long long* total, long long* available) override {
        if (!stream_) return -1;
        const int64_t size = stream_->size();
        if (total)     *total     = size;
        if (available) *available = size;
        return (size >= 0) ? 0 : -1;
    }

private:
    InputStream* stream_; // non-owning
};

// ---------------------------------------------------------------------------
// MatroskaDemuxer
// ---------------------------------------------------------------------------
class MatroskaDemuxer final : public BaseDemuxer {
public:
    MatroskaDemuxer()  = default;
    ~MatroskaDemuxer() override { close(); }

    // -----------------------------------------------------------------------
    // open()
    // -----------------------------------------------------------------------
    auto open(std::unique_ptr<InputStream> input) -> OMError override {
        input_ = std::move(input);
        if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;

        mkv_reader_ = std::make_unique<InputStreamMkvReader>(input_.get());

        // Parse the EBMLHeader — pos is updated to point just past the header
        long long pos = 0;
        mkvparser::EBMLHeader ebml_header;
        if (ebml_header.Parse(mkv_reader_.get(), pos) < 0)
            return OM_FORMAT_PARSE_FAILED;

        // Create and parse the Segment headers (not all clusters yet)
        mkvparser::Segment* raw_segment = nullptr;
        if (mkvparser::Segment::CreateInstance(mkv_reader_.get(), pos, raw_segment) != 0)
            return OM_FORMAT_PARSE_FAILED;

        segment_.reset(raw_segment);

        // ParseHeaders reads Tracks, Cues, SegmentInfo — stops before first cluster
        if (segment_->ParseHeaders() < 0)
            return OM_FORMAT_PARSE_FAILED;

        // Load tracks
        const mkvparser::Tracks* tracks = segment_->GetTracks();
        if (!tracks) return OM_FORMAT_PARSE_FAILED;

        if (OMError err = buildTrackMap(tracks); err != OM_SUCCESS)
            return err;

        // BUG FIX 1: Use a dedicated variable for LoadCluster, NOT the `pos`
        // variable from EBMLHeader parsing. Reusing `pos` would corrupt it
        // since LoadCluster mutates its first argument (it's an in/out param).
        // We pass 0 here to let the segment find the first cluster naturally.
        long long cluster_pos = 0;
        long      cluster_size = 0;
        if (segment_->LoadCluster(cluster_pos, cluster_size) < 0)
            return OM_FORMAT_PARSE_FAILED;

        current_cluster_     = segment_->GetFirst();
        current_block_entry_ = nullptr;

        return OM_SUCCESS;
    }

    // -----------------------------------------------------------------------
    // close()
    // -----------------------------------------------------------------------
    void close() override {
        while (!packet_queue_.empty()) packet_queue_.pop();
        current_block_entry_ = nullptr;
        current_cluster_     = nullptr;
        segment_.reset();
        mkv_reader_.reset();
        track_map_.clear();
        input_.reset();
    }

    // -----------------------------------------------------------------------
    // readPacket()
    // -----------------------------------------------------------------------
    auto readPacket() -> Result<Packet, OMError> override {
        // Drain any queued packets from multi-frame blocks first
        if (!packet_queue_.empty()) {
            Packet pkt = std::move(packet_queue_.front());
            packet_queue_.pop();
            return Ok(std::move(pkt));
        }

        while (true) {
            // BUG FIX 5: GetNext() can return nullptr (not just an EOS cluster)
            // when there are no more clusters. Must check for null BEFORE calling
            // ->EOS(), otherwise this is undefined behaviour (null dereference).
            if (!current_cluster_ || current_cluster_->EOS())
                return Err(OM_FORMAT_END_OF_FILE);

            // First call in a cluster: GetFirst; subsequent: GetNext
            const mkvparser::BlockEntry* entry = nullptr;
            if (!current_block_entry_) {
                if (current_cluster_->GetFirst(entry) < 0)
                    return Err(OM_FORMAT_PARSE_FAILED);
            } else {
                if (current_cluster_->GetNext(current_block_entry_, entry) < 0)
                    return Err(OM_FORMAT_PARSE_FAILED);
            }

            // End of this cluster → load and move to next
            if (!entry || entry->EOS()) {
                const mkvparser::Cluster* next = segment_->GetNext(current_cluster_);

                // BUG FIX 2: The original "fix" passed load_pos=0, which always
                // tries to load from byte offset 0 instead of the actual cluster's
                // file position. Use next->GetPosition() (the cluster's absolute file
                // offset within the segment data) so the correct cluster is loaded.
                if (next && !next->EOS()) {
                    long long load_pos  = next->GetPosition();
                    long      load_size = 0;
                    segment_->LoadCluster(load_pos, load_size);
                }

                current_cluster_     = next;
                current_block_entry_ = nullptr;
                continue;
            }

            current_block_entry_ = entry;

            const mkvparser::Block* block = entry->GetBlock();
            if (!block) continue;

            const long long track_num = block->GetTrackNumber();
            auto it = track_map_.find(static_cast<int32_t>(track_num));
            if (it == track_map_.end()) continue; // unknown / unsupported track

            const int32_t stream_index   = it->second;
            const int64_t timestamp_ns   = block->GetTime(current_cluster_);
            const bool    is_keyframe    = block->IsKey();

            const int frame_count = block->GetFrameCount();
            if (frame_count <= 0) continue;

            bool first = true;
            Packet first_pkt;

            for (int f = 0; f < frame_count; ++f) {
                const mkvparser::Block::Frame& frame = block->GetFrame(f);

                Packet pkt;
                pkt.allocate(static_cast<size_t>(frame.len));

                if (frame.Read(mkv_reader_.get(), pkt.bytes.data()) < 0)
                    return Err(OM_FORMAT_PARSE_FAILED);

                pkt.pts          = timestamp_ns;
                pkt.dts          = timestamp_ns;
                pkt.duration     = 0;
                pkt.stream_index = stream_index;
                pkt.is_keyframe  = is_keyframe;
                // BUG FIX 7: frame.pos is the absolute file offset of the
                // frame data in the raw stream. Assign it directly; do NOT
                // re-interpret it as a relative or timecode-based offset.
                pkt.pos          = static_cast<int64_t>(frame.pos);

                if (first) {
                    first_pkt = std::move(pkt);
                    first     = false;
                } else {
                    packet_queue_.push(std::move(pkt));
                }
            }

            return Ok(std::move(first_pkt));
        }
    }

    // -----------------------------------------------------------------------
    // seek()
    // -----------------------------------------------------------------------
    auto seek(int64_t timestamp_ns, int32_t stream_index) -> OMError override {
        if (!segment_) return OM_FORMAT_PARSE_FAILED;

        // Flush any buffered packets from the previous position
        while (!packet_queue_.empty()) packet_queue_.pop();

        // Find the track number for this stream index (reverse look-up)
        long long target_track_num = -1;
        for (const auto& [tnum, sidx] : track_map_) {
            if (sidx == stream_index) { target_track_num = tnum; break; }
        }

        const mkvparser::Cues* cues = segment_->GetCues();

        if (cues && target_track_num > 0) {
            // Preload all cue points
            while (!cues->DoneParsing())
                cues->LoadCuePoint();

            const mkvparser::Tracks* tracks = segment_->GetTracks();
            const mkvparser::Track*  track  =
                tracks->GetTrackByNumber(static_cast<unsigned long>(target_track_num));

            const mkvparser::CuePoint*                cue_point  = nullptr;
            const mkvparser::CuePoint::TrackPosition* track_pos  = nullptr;

            if (cues->Find(timestamp_ns, track, cue_point, track_pos) && track_pos) {
                // BUG FIX 4: track_pos->m_pos is a byte offset relative to the
                // Segment Data start, not an absolute cluster file offset.
                // FindOrPreloadCluster() expects an absolute position. Compute it
                // by adding the segment's data (element) start offset.
                const long long abs_pos =
                    segment_->m_start + static_cast<long long>(track_pos->m_pos);
                current_cluster_ = segment_->FindOrPreloadCluster(abs_pos);
            } else {
                current_cluster_ = segment_->GetFirst();
            }
        } else {
            // No cues — linear scan from the start
            current_cluster_ = segment_->GetFirst();

            // BUG FIX 3: GetTimeCode() returns timecodes in units of the
            // segment's timecode scale (default 1ms = 1,000,000 ns, but NOT
            // guaranteed). Multiplying blindly by 1,000,000 is wrong when the
            // scale differs. Convert properly using GetTimeCodeScale().
            const mkvparser::SegmentInfo* info = segment_->GetInfo();
            const long long timecode_scale =
                info ? info->GetTimeCodeScale() : 1000000LL; // default: 1ms

            while (current_cluster_ && !current_cluster_->EOS()) {
                const long long cluster_ns =
                    current_cluster_->GetTimeCode() * timecode_scale;
                if (cluster_ns >= timestamp_ns)
                    break;
                current_cluster_ = segment_->GetNext(current_cluster_);
            }
        }

        current_block_entry_ = nullptr;
        return OM_SUCCESS;
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /// Walk all tracks and build track_num → stream_index mapping.
    OMError buildTrackMap(const mkvparser::Tracks* tracks) {
        const unsigned long count = tracks->GetTracksCount();
        int32_t next_index = 0;

        for (unsigned long i = 0; i < count; ++i) {
            const mkvparser::Track* t = tracks->GetTrackByIndex(i);
            if (!t) continue;

            const long type = t->GetType();

            if (type == mkvparser::Track::kVideo) {
                const auto* vt = static_cast<const mkvparser::VideoTrack*>(t);

                Track track{};
                track.index              = next_index;
                track.id                 = static_cast<int32_t>(t->GetNumber());
                track.format.type        = OM_MEDIA_VIDEO;
                track.format.codec_id    = mkvCodecIdToOMCodec(t->GetCodecId());
                track.format.video.width  = static_cast<uint32_t>(vt->GetWidth());
                track.format.video.height = static_cast<uint32_t>(vt->GetHeight());
                const double fps = vt->GetFrameRate();
                track.format.video.framerate = fps > 0
                    ? Rational{static_cast<int32_t>(fps), 1}
                    : Rational{0, 1};

                size_t cp_size = 0;
                const unsigned char* cp = t->GetCodecPrivate(cp_size);
                if (cp && cp_size)
                    track.extradata.assign(cp, cp + cp_size);

                tracks_.push_back(track);
                track_map_[static_cast<int32_t>(t->GetNumber())] = next_index++;

            } else if (type == mkvparser::Track::kAudio) {
                const auto* at = static_cast<const mkvparser::AudioTrack*>(t);

                Track track{};
                track.index              = next_index;
                track.id                 = static_cast<int32_t>(t->GetNumber());
                track.format.type        = OM_MEDIA_AUDIO;
                track.format.codec_id    = mkvCodecIdToOMCodec(t->GetCodecId());
                track.format.audio.sample_rate = static_cast<uint32_t>(at->GetSamplingRate());
                track.format.audio.channels    = static_cast<uint32_t>(at->GetChannels());
                track.format.audio.bit_depth   = static_cast<uint32_t>(at->GetBitDepth());

                size_t cp_size = 0;
                const unsigned char* cp = t->GetCodecPrivate(cp_size);
                if (cp && cp_size)
                    track.extradata.assign(cp, cp + cp_size);

                tracks_.push_back(track);
                track_map_[static_cast<int32_t>(t->GetNumber())] = next_index++;
            }
            // Subtitle / metadata tracks are silently ignored for now.
        }

        return OM_SUCCESS;
    }

    /// Map a libwebm codec string to the openmedia codec identifier.
    static OMCodecId mkvCodecIdToOMCodec(const char* id) {
        if (!id) return OM_CODEC_NONE;

        // Video
        if (std::strcmp(id, "V_VP8")             == 0) return OM_CODEC_VP8;
        if (std::strcmp(id, "V_VP9")             == 0) return OM_CODEC_VP9;
        if (std::strcmp(id, "V_AV1")             == 0) return OM_CODEC_AV1;
        if (std::strcmp(id, "V_MPEG4/ISO/AVC")   == 0) return OM_CODEC_H264;
        if (std::strcmp(id, "V_MPEGH/ISO/HEVC")  == 0) return OM_CODEC_H265;
        if (std::strcmp(id, "V_MPEG2")            == 0) return OM_CODEC_MPEG2;

        // Audio
        if (std::strcmp(id, "A_OPUS")            == 0) return OM_CODEC_OPUS;
        if (std::strcmp(id, "A_VORBIS")          == 0) return OM_CODEC_VORBIS;
        if (std::strcmp(id, "A_AAC")             == 0) return OM_CODEC_AAC;
        if (std::strcmp(id, "A_MPEG/L3")         == 0) return OM_CODEC_MP3;
        if (std::strcmp(id, "A_FLAC")            == 0) return OM_CODEC_FLAC;
        if (std::strcmp(id, "A_PCM/INT/LIT")     == 0) return OM_CODEC_PCM_S16LE;
        if (std::strcmp(id, "A_PCM/INT/BIG")     == 0) return OM_CODEC_PCM_S16BE;
        if (std::strcmp(id, "A_AC3")             == 0) return OM_CODEC_AC3;
        if (std::strcmp(id, "A_EAC3")            == 0) return OM_CODEC_EAC3;

        return OM_CODEC_NONE;
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    std::unique_ptr<InputStream>           input_;
    std::unique_ptr<InputStreamMkvReader>  mkv_reader_;
    std::unique_ptr<mkvparser::Segment>    segment_;

    /// libwebm track number → openmedia stream index
    std::map<int32_t, int32_t>             track_map_;

    /// Overflow queue for multi-frame blocks (frames 1..N held here)
    std::queue<Packet>                     packet_queue_;

    /// Current position in the cluster / block iterator
    const mkvparser::Cluster*              current_cluster_     = nullptr;
    const mkvparser::BlockEntry*           current_block_entry_ = nullptr;
};

// ---------------------------------------------------------------------------
// Public factory + descriptor
// ---------------------------------------------------------------------------
auto create_matroska_demuxer() -> std::unique_ptr<Demuxer> {
    return std::make_unique<MatroskaDemuxer>();
}

const FormatDescriptor FORMAT_MATROSKA = {
    .container_id    = OM_CONTAINER_MKV,
    .name            = "matroska",
    .long_name       = "Matroska / WebM",
    .demuxer_factory = [] { return create_matroska_demuxer(); },
    .muxer_factory   = {},
};

} // namespace openmedia