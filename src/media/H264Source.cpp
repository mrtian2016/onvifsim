#include "media/H264Source.h"

#include <QtCore/QFile>
#include <QtCore/QStringList>

#include <cmath>

// 静态库里的 qrc 必须被显式点名一次。链接器只会从 .a 里捞出「有人引用的」目标文件，
// 而 rcc 生成的资源对象谁也不引用，不点名就直接被丢掉，运行期表现为 ":/media/*" 全部打不开。
// AudioSource.cpp 里的 AAC 样片也走这一份，所以这个函数不是 static。
void onvifsimInitMediaResources()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    Q_INIT_RESOURCE(assets);
}

namespace onvifsim {
namespace {

// ---- 内嵌样片清单 -------------------------------------------------------
// 档位名与 assets/media/ 下的文件名分开写：档位名会出现在场景文件和 REST 里，
// 改文件名不该逼着用户改场景。
struct BuiltinAsset {
    const char *name;
    const char *resource;
};

const BuiltinAsset kBuiltins[] = {
    { "360p",   ":/media/h264-360p.h264" },
    { "720p",   ":/media/h264-720p.h264" },
    { "1080p",  ":/media/h264-1080p.h264" },
    { "black",  ":/media/h264-black.h264" },
    { "freeze", ":/media/h264-freeze.h264" },
    { "noise",  ":/media/h264-noise.h264" },
};

QString builtinResourcePath(const QString &name)
{
    for (const BuiltinAsset &a : kBuiltins) {
        if (name == QLatin1String(a.name))
            return QString::fromLatin1(a.resource);
    }
    return QString();
}

// 样片是 15 fps 生成的；万一某个外部 .h264 的 SPS 里没写 VUI timing，就退到这个值，
// 总比 durationUs() 除以零强。
constexpr double kFallbackFrameRate = 15.0;

bool isVclNalType(quint8 type)
{
    return type >= 1 && type <= 5;
}

// ---- RBSP 与比特读取 ----------------------------------------------------

QByteArray rbspFromNal(const QByteArray &nal)
{
    // 去掉 emulation prevention 的 0x03（出现在 00 00 03 之后）。
    // 不去的话 exp-Golomb 会多读到一串假零，宽高解出来能差好几个宏块。
    QByteArray out;
    out.reserve(nal.size());
    int zeros = 0;
    for (int i = 0; i < nal.size(); ++i) {
        const quint8 b = static_cast<quint8>(nal.at(i));
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        out.append(static_cast<char>(b));
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

class BitReader
{
public:
    explicit BitReader(const QByteArray &data)
        : m_data(data), m_totalBits(data.size() * 8)
    {
    }

    bool ok() const { return m_ok; }

    quint32 bit()
    {
        if (m_pos >= m_totalBits) {
            m_ok = false;
            return 0;
        }
        const int index = static_cast<int>(m_pos >> 3);
        const int shift = 7 - static_cast<int>(m_pos & 7);
        ++m_pos;
        return (static_cast<quint8>(m_data.at(index)) >> shift) & 1u;
    }

    quint32 u(int count)
    {
        quint32 value = 0;
        for (int i = 0; i < count; ++i)
            value = (value << 1) | bit();
        return value;
    }

    quint32 ue()
    {
        int leadingZeros = 0;
        while (m_ok) {
            if (bit() == 1)
                break;
            if (++leadingZeros > 31) {
                // 32 个前导零意味着码流已经跑飞了，再读下去只会得到垃圾。
                m_ok = false;
                return 0;
            }
        }
        if (!m_ok || leadingZeros == 0)
            return 0;
        return ((1u << leadingZeros) - 1u) + u(leadingZeros);
    }

    int se()
    {
        const quint32 k = ue();
        return (k & 1u) ? static_cast<int>((k + 1) / 2) : -static_cast<int>(k / 2);
    }

private:
    QByteArray m_data;
    qint64 m_totalBits = 0;
    qint64 m_pos = 0;
    bool m_ok = true;
};

void skipScalingList(BitReader &r, int size)
{
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size && r.ok(); ++i) {
        if (nextScale != 0)
            nextScale = (lastScale + r.se() + 256) % 256;
        if (nextScale != 0)
            lastScale = nextScale;
    }
}

struct SpsInfo {
    bool valid = false;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
};

// 只解到「算得出宽高帧率」为止：后面的 HRD 参数对模拟器没用，解错了还容易把
// 前面已经解对的结果带偏，所以读到 timing_info 就收手。
SpsInfo parseSps(const QByteArray &nal)
{
    SpsInfo info;
    const QByteArray rbsp = rbspFromNal(nal);
    if (rbsp.size() < 4)
        return info;

    BitReader r(rbsp);
    r.u(8);                                     // nal_unit_header
    const quint32 profileIdc = r.u(8);
    r.u(8);                                     // constraint_set flags + reserved
    r.u(8);                                     // level_idc
    r.ue();                                     // seq_parameter_set_id

    quint32 chromaFormatIdc = 1;                // 缺省 4:2:0
    bool separateColourPlane = false;
    switch (profileIdc) {
    case 100: case 110: case 122: case 244: case 44:
    case 83:  case 86:  case 118: case 128:
    case 138: case 139: case 134: case 135:
        chromaFormatIdc = r.ue();
        if (chromaFormatIdc == 3)
            separateColourPlane = r.u(1) != 0;
        r.ue();                                 // bit_depth_luma_minus8
        r.ue();                                 // bit_depth_chroma_minus8
        r.u(1);                                 // qpprime_y_zero_transform_bypass_flag
        if (r.u(1)) {                           // seq_scaling_matrix_present_flag
            const int lists = (chromaFormatIdc != 3) ? 8 : 12;
            for (int i = 0; i < lists && r.ok(); ++i) {
                if (r.u(1))
                    skipScalingList(r, i < 6 ? 16 : 64);
            }
        }
        break;
    default:
        break;
    }

    r.ue();                                     // log2_max_frame_num_minus4
    const quint32 pocType = r.ue();
    if (pocType == 0) {
        r.ue();                                 // log2_max_pic_order_cnt_lsb_minus4
    } else if (pocType == 1) {
        r.u(1);                                 // delta_pic_order_always_zero_flag
        r.se();                                 // offset_for_non_ref_pic
        r.se();                                 // offset_for_top_to_bottom_field
        const quint32 cycle = r.ue();
        for (quint32 i = 0; i < cycle && r.ok(); ++i)
            r.se();
    }
    r.ue();                                     // max_num_ref_frames
    r.u(1);                                     // gaps_in_frame_num_value_allowed_flag

    const quint32 widthInMbs = r.ue() + 1;
    const quint32 heightInMapUnits = r.ue() + 1;
    const bool frameMbsOnly = r.u(1) != 0;
    if (!frameMbsOnly)
        r.u(1);                                 // mb_adaptive_frame_field_flag
    r.u(1);                                     // direct_8x8_inference_flag

    quint32 cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (r.u(1)) {                               // frame_cropping_flag
        cropLeft = r.ue();
        cropRight = r.ue();
        cropTop = r.ue();
        cropBottom = r.ue();
    }
    if (!r.ok())
        return info;

    int width = static_cast<int>(widthInMbs) * 16;
    int height = static_cast<int>(heightInMapUnits) * 16 * (frameMbsOnly ? 1 : 2);

    // 裁剪单位随色度格式变：4:2:0 横竖都是 2，单色则是 1。1080p 就靠这里减掉 8 行。
    const quint32 chromaArrayType = separateColourPlane ? 0u : chromaFormatIdc;
    int cropUnitX = 1;
    int cropUnitY = frameMbsOnly ? 1 : 2;
    if (chromaArrayType != 0) {
        const int subWidthC = (chromaArrayType == 3) ? 1 : 2;
        const int subHeightC = (chromaArrayType == 1) ? 2 : 1;
        cropUnitX = subWidthC;
        cropUnitY = subHeightC * (frameMbsOnly ? 1 : 2);
    }
    width -= static_cast<int>(cropLeft + cropRight) * cropUnitX;
    height -= static_cast<int>(cropTop + cropBottom) * cropUnitY;
    if (width <= 0 || height <= 0)
        return info;

    if (r.u(1)) {                               // vui_parameters_present_flag
        if (r.u(1)) {                           // aspect_ratio_info_present_flag
            if (r.u(8) == 255) {                // Extended_SAR
                r.u(16);
                r.u(16);
            }
        }
        if (r.u(1))
            r.u(1);                             // overscan_appropriate_flag
        if (r.u(1)) {                           // video_signal_type_present_flag
            r.u(3);                             // video_format
            r.u(1);                             // video_full_range_flag
            if (r.u(1)) {                       // colour_description_present_flag
                r.u(8);
                r.u(8);
                r.u(8);
            }
        }
        if (r.u(1)) {                           // chroma_loc_info_present_flag
            r.ue();
            r.ue();
        }
        if (r.u(1)) {                           // timing_info_present_flag
            const quint32 numUnitsInTick = r.u(32);
            const quint32 timeScale = r.u(32);
            r.u(1);                             // fixed_frame_rate_flag
            // 一帧占两个 tick（field 计时），所以要除以 2 —— x264 写的正是 time_scale = 2*fps。
            if (r.ok() && numUnitsInTick > 0 && timeScale > 0)
                info.frameRate = static_cast<double>(timeScale)
                    / (2.0 * static_cast<double>(numUnitsInTick));
        }
    }

    info.width = width;
    info.height = height;
    info.valid = true;
    return info;
}

// ---- Annex B 扫描 -------------------------------------------------------

struct NalRange {
    int startCode = 0;      // 起始码第一个字节的偏移
    int payload = 0;        // NAL 头（含 nal_unit_type）的偏移
    int payloadSize = 0;
};

QVector<NalRange> scanAnnexB(const char *data, int size)
{
    QVector<NalRange> out;
    const quint8 *p = reinterpret_cast<const quint8 *>(data);
    NalRange current;
    bool haveCurrent = false;
    int i = 0;
    while (i + 2 < size) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int startCode = i;
            // 四字节起始码就是三字节起始码前面再补一个 0。多吞前一个 NAL 的尾字节是安全的：
            // RBSP 末尾必有 rbsp_stop_one_bit，最后一个字节不可能是 0x00。
            if (startCode > 0 && p[startCode - 1] == 0)
                --startCode;
            if (haveCurrent) {
                current.payloadSize = startCode - current.payload;
                if (current.payloadSize > 0)
                    out.append(current);
            }
            current.startCode = startCode;
            current.payload = i + 3;
            haveCurrent = true;
            i += 3;
            continue;
        }
        ++i;
    }
    if (haveCurrent) {
        current.payloadSize = size - current.payload;
        if (current.payloadSize > 0)
            out.append(current);
    }
    return out;
}

} // namespace

// ---- Private ------------------------------------------------------------

struct H264Source::Private {
    // 帧表存偏移而不是拷贝：1080p 样片 1.7 MB，每帧再拷一份纯属浪费。
    // 定义在 Private 里面而不是匿名 namespace 里，是为了避开 -Wsubobject-linkage。
    struct FrameRange {
        int offset = 0;         // 含起始码，直接 mid() 出去就是合法 Annex B
        int size = 0;
        bool keyFrame = false;
        bool hasParameterSet = false;
    };

    bool loaded = false;
    QString assetName;
    QByteArray data;
    QVector<FrameRange> frames;
    QByteArray sps;
    QByteArray pps;
    int width = 0;
    int height = 0;
    double frameRate = kFallbackFrameRate;
};

H264Source::H264Source()
    : d(new Private)
{
}

H264Source::~H264Source()
{
    delete d;
}

bool H264Source::load(const QString &asset, QString *errorOut)
{
    onvifsimInitMediaResources();

    const QString builtin = builtinResourcePath(asset);
    const QString path = builtin.isEmpty() ? asset : builtin;
    if (path.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("样片名为空");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("打不开样片 %1：%2").arg(path, file.errorString());
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();
    if (data.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("样片 %1 是空的").arg(path);
        return false;
    }

    // 先全部解析到局部变量，成功了再落到 d 上：半截失败不该把已经在播的源打坏。
    const QVector<NalRange> nals = scanAnnexB(data.constData(), data.size());
    if (nals.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("样片 %1 里找不到 Annex B 起始码").arg(path);
        return false;
    }

    QByteArray sps;
    QByteArray pps;
    QVector<Private::FrameRange> frames;
    int frameStart = nals.first().startCode;
    bool frameHasParameterSet = false;
    for (const NalRange &nal : nals) {
        const quint8 type = static_cast<quint8>(data.at(nal.payload)) & 0x1F;
        if (type == 7 && sps.isEmpty())
            sps = data.mid(nal.payload, nal.payloadSize);
        else if (type == 8 && pps.isEmpty())
            pps = data.mid(nal.payload, nal.payloadSize);
        if (type == 7 || type == 8)
            frameHasParameterSet = true;

        if (!isVclNalType(type))
            continue;

        // 一帧 = 一个 VCL NAL 加上它前面所有非 VCL NAL（SPS/PPS/SEI）。
        // 样片是每帧单 slice 的，所以不必再去解 slice header 里的 first_mb_in_slice。
        const int frameEnd = nal.payload + nal.payloadSize;
        Private::FrameRange fr;
        fr.offset = frameStart;
        fr.size = frameEnd - frameStart;
        fr.keyFrame = (type == 5);
        fr.hasParameterSet = frameHasParameterSet;
        frames.append(fr);
        frameStart = frameEnd;
        frameHasParameterSet = false;
    }

    if (frames.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("样片 %1 里没有 VCL NAL").arg(path);
        return false;
    }
    if (sps.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("样片 %1 里没有 SPS，出不了 sprop-parameter-sets").arg(path);
        return false;
    }

    const SpsInfo info = parseSps(sps);
    if (!info.valid) {
        if (errorOut)
            *errorOut = QStringLiteral("样片 %1 的 SPS 解不出分辨率").arg(path);
        return false;
    }

    d->loaded = true;
    d->assetName = asset;
    d->data = data;
    d->frames = frames;
    d->sps = sps;
    d->pps = pps;
    d->width = info.width;
    d->height = info.height;
    d->frameRate = info.frameRate > 0.0 ? info.frameRate : kFallbackFrameRate;
    return true;
}

bool H264Source::isLoaded() const
{
    return d->loaded;
}

QString H264Source::assetName() const
{
    return d->assetName;
}

int H264Source::width() const
{
    return d->width;
}

int H264Source::height() const
{
    return d->height;
}

double H264Source::frameRate() const
{
    return d->frameRate;
}

qint64 H264Source::durationUs() const
{
    if (!d->loaded || d->frameRate <= 0.0)
        return 0;
    return std::llround(d->frames.size() * 1000000.0 / d->frameRate);
}

int H264Source::frameCount() const
{
    return d->frames.size();
}

QByteArray H264Source::sps() const
{
    return d->sps;
}

QByteArray H264Source::pps() const
{
    return d->pps;
}

QByteArray H264Source::spropParameterSets() const
{
    if (d->sps.isEmpty())
        return QByteArray();
    QByteArray out = d->sps.toBase64();
    if (!d->pps.isEmpty())
        out += ',' + d->pps.toBase64();
    return out;
}

QByteArray H264Source::profileLevelId() const
{
    // SPS 的 NAL 头之后紧跟 profile_idc / constraint_flags / level_idc，正好三字节。
    if (d->sps.size() < 4)
        return QByteArray();
    return d->sps.mid(1, 3).toHex().toUpper();
}

EncodedFrame H264Source::frame(int index) const
{
    EncodedFrame out;
    if (!d->loaded || d->frames.isEmpty())
        return out;

    const int count = d->frames.size();
    int wrapped = index % count;
    if (wrapped < 0)
        wrapped += count;       // C++ 的 % 对负数向零取整，直接用会取到负下标

    const Private::FrameRange &fr = d->frames.at(wrapped);
    out.data = d->data.mid(fr.offset, fr.size);
    out.keyFrame = fr.keyFrame;
    out.isParameterSet = fr.hasParameterSet;
    // pts 用没取模的 index 算：循环播放时时间戳必须一路递增。
    // 一旦回绕，客户端会当成时间戳跳变，轻则丢缓冲重排，重则直接断流重连。
    out.ptsUs = std::llround(static_cast<double>(index) * 1000000.0 / d->frameRate);
    return out;
}

QVector<QByteArray> H264Source::splitNalUnits(const QByteArray &annexB)
{
    QVector<QByteArray> out;
    const QVector<NalRange> nals = scanAnnexB(annexB.constData(), annexB.size());
    out.reserve(nals.size());
    for (const NalRange &nal : nals)
        out.append(annexB.mid(nal.payload, nal.payloadSize));
    return out;
}

QVector<QByteArray> H264Source::packetize(const QByteArray &nal, int mtu)
{
    QVector<QByteArray> out;
    // FU-A 每包要 2 字节头，mtu 小于 3 连一个字节负载都塞不下，直接判无解。
    if (nal.isEmpty() || mtu < 3)
        return out;

    if (nal.size() <= mtu) {
        out.append(nal);        // 单 NAL 包：整个 NAL（含 nal 头）就是 RTP 负载
        return out;
    }

    const quint8 header = static_cast<quint8>(nal.at(0));
    const quint8 indicator = static_cast<quint8>((header & 0xE0) | 28);   // F/NRI 照抄，type=28
    const quint8 baseType = static_cast<quint8>(header & 0x1F);
    const int perPacket = mtu - 2;

    const int end = nal.size();
    int pos = 1;                // NAL 头不进分片负载，它的信息拆进了 indicator 和 FU header
    out.reserve((end - 1 + perPacket - 1) / perPacket);
    while (pos < end) {
        const int chunk = qMin(perPacket, end - pos);
        quint8 fuHeader = baseType;
        if (pos == 1)
            fuHeader |= 0x80;   // S
        if (pos + chunk >= end)
            fuHeader |= 0x40;   // E
        QByteArray packet;
        packet.reserve(chunk + 2);
        packet.append(static_cast<char>(indicator));
        packet.append(static_cast<char>(fuHeader));
        packet.append(nal.constData() + pos, chunk);
        out.append(packet);
        pos += chunk;
    }
    return out;
}

QStringList H264Source::builtinAssets()
{
    QStringList out;
    out.reserve(static_cast<int>(sizeof(kBuiltins) / sizeof(kBuiltins[0])));
    for (const BuiltinAsset &a : kBuiltins)
        out.append(QString::fromLatin1(a.name));
    return out;
}

} // namespace onvifsim
