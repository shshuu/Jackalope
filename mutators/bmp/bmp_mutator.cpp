#include "bmp_mutator.h"
#include "mutator.h"
#include "sample.h"
#include "prng.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>

// Signed Shift 문제를 방지한 Little-Endian 안전 읽기/쓰기 헬퍼 함수
static inline uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
        (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}
static inline int32_t ReadLE32S(const uint8_t* p) {
    return static_cast<int32_t>(ReadLE32(p));
}
static inline void WriteLE16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static inline void WriteLE32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline void WriteLE32S(uint8_t* p, int32_t v) {
    WriteLE32(p, static_cast<uint32_t>(v));
}

// 안전한 가로 폭 Stride 계산식 (오버플로우 방지)
static inline uint64_t CalculateStride(int32_t width) {
    if (width <= 0) return 0;
    return ((static_cast<uint64_t>(width) * 8 + 31) / 32) * 4;
}

// INT32_MIN 언더플로우를 방지하는 안전한 절대값 계산
static inline uint64_t SafeAbsHeight(int32_t height) {
    int64_t h64 = height;
    return (h64 < 0) ? -h64 : h64;
}

// Jackalope 공식 가상 함수 인터페이스 오버라이딩 바디 구현

bool BmpMutator::Mutate(Sample* inout_sample, PRNG* prng,
    std::vector<Sample*>& all_samples) {
    const int32_t MAX_BMP_DIMENSION = 8192; // GDI+ 파싱 타임아웃 및 메모리 고갈 방지 상한선

    // 1. 최소 샘플 헤더 데이터 유효성 검증
    if (!inout_sample || inout_sample->size < 54) return false;

    uint8_t* pFileHeader = reinterpret_cast<uint8_t*>(inout_sample->bytes);
    uint8_t* pInfoHeader = reinterpret_cast<uint8_t*>(inout_sample->bytes + 14);

    // Strict 8bpp BI_RGB 포맷 필터링 (GDI+ 파서 조기 통과 목적)
    if (ReadLE16(pFileHeader + 0) != 0x4D42) return false;       // bfType != 'BM'
    if (ReadLE32(pInfoHeader + 0) != 40) return false;         // biSize != 40
    if (ReadLE16(pInfoHeader + 12) != 1) return false;         // biPlanes != 1
    if (ReadLE16(pInfoHeader + 14) != 8) return false;         // biBitCount != 8
    if (ReadLE32(pInfoHeader + 16) != 0) return false;         // biCompression != 0 (BI_RGB)

    uint32_t bfOffBits = ReadLE32(pFileHeader + 10);
    uint32_t biClrUsed = ReadLE32(pInfoHeader + 32);
    uint32_t clrUsed = (biClrUsed == 0) ? 256 : biClrUsed;
    if (clrUsed > 256) return false;

    // 팔레트 바운더리 체크 및 데이터 오프셋 검증
    uint32_t minOffBits = 14 + 40 + (clrUsed * 4);
    if (bfOffBits < minOffBits || bfOffBits > inout_sample->size) return false;

    int32_t width = ReadLE32S(pInfoHeader + 4);
    int32_t height = ReadLE32S(pInfoHeader + 8);
    uint64_t absHeight = SafeAbsHeight(height);

    if (width <= 0 || width > MAX_BMP_DIMENSION) return false;
    if (absHeight <= 0 || absHeight > MAX_BMP_DIMENSION) return false;

    uint64_t stride = CalculateStride(width);
    uint64_t calculatedSizeImage = stride * absHeight;
    if (calculatedSizeImage > 0x7FFFFFFF) return false;

    // [보완 요구사항] 기존 입력 시드의 biSizeImage가 잘못 변형된 상태(0이 아니면서 불일치)라면 거부
    uint32_t biSizeImage = ReadLE32(pInfoHeader + 20);
    if (biSizeImage != 0 && biSizeImage != calculatedSizeImage) {
        return false;
    }

    // 구조 유효 검증을 통과한 확실한 계산 이미지 크기를 변이 베이스로 사용
    uint64_t currentPixelAreaSize = calculatedSizeImage;

    // 물리 메모리 오버런 방지를 위한 페이로드 오프셋 바운드 체크
    if (bfOffBits + currentPixelAreaSize > inout_sample->size) {
        return false;
    }

    bool mutationSuccess = false;
    int attempts = 0;

    while (!mutationSuccess && attempts++ < 5) {
        // [확장 전략] 90% 확률로 구조 유효 변이 수행, 10% 확률로 의도적 구조 파괴(불일치) 변이 수행
        int modeRoll = prng->Rand(1, 100);

        if (modeRoll <= 90) {
            // ==========================================
            // [A. 유효 모드 - 90%] Coverage 확장 위주
            // ==========================================
            int mutationType = prng->Rand(0, 3);

            if (mutationType == 0) {
                // [변이 1] 픽셀 데이터 무작위 변이 (유효 팔레트 인덱스 범위 준수)
                if (currentPixelAreaSize > 0) {
                    int maxMutate = static_cast<int>((std::min)(static_cast<uint64_t>(currentPixelAreaSize), 15ULL));
                    int mutateCount = prng->Rand(1, maxMutate);

                    for (int i = 0; i < mutateCount; ++i) {
                        uint64_t targetOffset = static_cast<uint64_t>(prng->Rand(0, static_cast<int>(currentPixelAreaSize - 1)));
                        uint64_t colIdx = targetOffset % stride;

                        if (colIdx < static_cast<uint64_t>(width)) {
                            inout_sample->bytes[bfOffBits + targetOffset] = static_cast<char>(prng->Rand(0, static_cast<int>(clrUsed - 1)));
                        }
                        else {
                            inout_sample->bytes[bfOffBits + targetOffset] = static_cast<char>(prng->Rand(0, 255));
                        }
                    }
                    mutationSuccess = true;
                }
            }
            else if (mutationType == 1) {
                // [변이 2] 팔레트 B/G/R 테이블 변이
                int targetPaletteIdx = prng->Rand(0, static_cast<int>(clrUsed - 1));
                uint64_t paletteElementOffset = 14 + 40 + (static_cast<uint64_t>(targetPaletteIdx) * 4) + static_cast<uint64_t>(prng->Rand(0, 2));
                inout_sample->bytes[paletteElementOffset] = static_cast<char>(prng->Rand(0, 255));
                mutationSuccess = true;
            }
            else if (mutationType == 2) {
                // [변이 3] Padding 영역 명시적 변이 (구조 유지)
                uint32_t paddingSize = static_cast<uint32_t>(stride - width);
                if (paddingSize > 0) {
                    int targetRow = prng->Rand(0, static_cast<int>(absHeight) - 1);
                    int targetPadIdx = prng->Rand(0, static_cast<int>(paddingSize - 1));
                    uint64_t targetOffset = (static_cast<uint64_t>(targetRow) * stride) + static_cast<uint64_t>(width) + static_cast<uint64_t>(targetPadIdx);

                    if (targetOffset < currentPixelAreaSize) {
                        inout_sample->bytes[bfOffBits + targetOffset] = static_cast<char>(prng->Rand(0, 255));
                        mutationSuccess = true;
                    }
                }
            }
            else if (mutationType == 3) {
                // [변이 4] 가로/세로 크기 변경 및 물리 픽셀 행 재배치 (모든 연동 필드 완벽 동기화)
                int32_t newWidth = static_cast<int32_t>(prng->Rand(4, 256));
                int32_t newHeight = static_cast<int32_t>(prng->Rand(4, 256));
                if (height < 0) newHeight = -newHeight;

                uint64_t newStride = CalculateStride(newWidth);
                uint64_t newAbsHeight = SafeAbsHeight(newHeight);
                uint64_t newSizeImage = newStride * newAbsHeight;
                uint64_t newBfSize = bfOffBits + newSizeImage;

                if (newBfSize > Sample::max_size) {
                    continue;
                }

                std::vector<uint8_t> newPixelArray(newSizeImage, 0);
                uint32_t copyRows = (std::min)(static_cast<uint32_t>(absHeight), static_cast<uint32_t>(newAbsHeight));
                uint32_t copyPixels = (std::min)(static_cast<uint32_t>(width), static_cast<uint32_t>(newWidth));

                for (uint32_t r = 0; r < copyRows; ++r) {
                    const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(inout_sample->bytes + bfOffBits + (r * stride));
                    uint8_t* dstRow = newPixelArray.data() + (r * newStride);
                    std::memcpy(dstRow, srcRow, copyPixels);

                    if (static_cast<uint32_t>(newWidth) > copyPixels) {
                        for (uint32_t w = copyPixels; w < static_cast<uint32_t>(newWidth); ++w) {
                            dstRow[w] = static_cast<uint8_t>(prng->Rand(0, static_cast<int>(clrUsed - 1)));
                        }
                    }
                }

                inout_sample->Resize(newBfSize);

                pFileHeader = reinterpret_cast<uint8_t*>(inout_sample->bytes);
                pInfoHeader = reinterpret_cast<uint8_t*>(inout_sample->bytes + 14);

                std::memcpy(inout_sample->bytes + bfOffBits, newPixelArray.data(), newSizeImage);

                WriteLE32S(pInfoHeader + 4, newWidth);
                WriteLE32S(pInfoHeader + 8, newHeight);
                WriteLE32(pInfoHeader + 20, static_cast<uint32_t>(newSizeImage));
                WriteLE32(pFileHeader + 2, static_cast<uint32_t>(newBfSize));

                mutationSuccess = true;
            }
        }
        else {
            // ==========================================
            // [B. 불일치 모드 - 10%] 오류 처리 및 취약점 유도
            // ==========================================
            int malformedType = prng->Rand(0, 2);

            if (malformedType == 0) {
                uint32_t fakeSizeImage =
                    static_cast<uint32_t>(currentPixelAreaSize);

                if (prng->Rand(0, 1) == 0) {
                    fakeSizeImage = (fakeSizeImage > 10)
                        ? fakeSizeImage - prng->Rand(1, 10)
                        : 1;
                }
                else {
                    fakeSizeImage += prng->Rand(1, 1000);
                }

                WriteLE32(pInfoHeader + 20, fakeSizeImage);
                mutationSuccess = true;
            }
            else if (malformedType == 1) {
                uint32_t fakeBfSize =
                    static_cast<uint32_t>(inout_sample->size);

                if (prng->Rand(0, 1) == 0) {
                    fakeBfSize = (fakeBfSize > 10)
                        ? fakeBfSize - prng->Rand(1, 10)
                        : 1;
                }
                else {
                    fakeBfSize += prng->Rand(1, 1000);
                }

                WriteLE32(pFileHeader + 2, fakeBfSize);
                mutationSuccess = true;
            }
            else {
                uint32_t badClrUsed = clrUsed + prng->Rand(1, 500);
                WriteLE32(pInfoHeader + 32, badClrUsed);
                mutationSuccess = true;
            }
        }
    }
    return mutationSuccess;
}