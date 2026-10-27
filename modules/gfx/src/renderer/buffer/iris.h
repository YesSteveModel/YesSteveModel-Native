#pragma once

#include <cmath>
#include <cstring>
#include <span>

#include "cpu.h"
#include "empty.h"
#include "non_copyable.h"
#include "quad_ref.h"
#include "gfx/renderer/render.h"

namespace ysm::gfx::renderer::buffer {
    template<typename VertexType, simd::Type kSimdType, bool kIsShadow, simd::Width kVecWidth = simd::width<kSimdType>()>
    class alignas(kIsShadow ? 8 : simd::bytes(kVecWidth)) IrisVertexWriter : NonCopyable {
        const std::span<VertexType> vertices_;

        std::span<VertexType, 4> current_quad_{static_cast<VertexType *>(nullptr), 4};
        uint32_t quad_ptr_ = 0;

    public:
        explicit IrisVertexWriter(std::span<VertexType> vertices) noexcept
                : vertices_(vertices) {
        }

        void BeginQuad() {
            current_quad_ = vertices_.subspan(quad_ptr_ * 4, 4).template subspan<0, 4>();
        }

        VertexType& QuadVertex(size_t index) const {
            auto &vertex = current_quad_[index];
            if constexpr (kIsShadow) {
                std::memset(&vertex.color, 0, sizeof(VertexType) - sizeof(vertex.pos));
            }
            return vertex;
        }

        void EndQuad() {
            ++quad_ptr_;
        }

        ~IrisVertexWriter() {
            if (quad_ptr_ * 4 < vertices_.size()) {
                auto fillVertices = vertices_.subspan(quad_ptr_ * 4);
                std::memset(fillVertices.data(), 0, fillVertices.size() * sizeof(VertexType));
            }
        }
    };

    template<typename VertexTypeIn, simd::Type kSimdType, bool kIsShadow>
    class IrisBuffer {
        const std::span<VertexTypeIn> vertices_;

    public:
        using VertexType = VertexTypeIn;
        using WriterType = IrisVertexWriter<VertexType, kSimdType, kIsShadow>;

        constexpr static size_t kVertexSize = sizeof(VertexType);
        constexpr static bool kTranslucent = false;
        constexpr static bool kIris = true;
        constexpr static bool kPosOnly = kIsShadow;

        IrisBuffer(std::span<VertexType> vertices, const RenderParameters& params) :
                vertices_(vertices) {
        }

        auto CreateVertexWriter(uint32_t vertex_offset, uint32_t expectedVertexCount) const {
            auto vertices = vertices_.subspan(vertex_offset, expectedVertexCount);
            return IrisVertexWriter<VertexType, kSimdType, kIsShadow>{vertices};
        }

        auto CreateVertexWriter(const std::span<VertexType>& vertices) const noexcept {
            return IrisVertexWriter<VertexType, kSimdType, kIsShadow>{vertices};
        }

        void WriteSortedQuads(const std::span<VertexType> &vertices_in, const std::span<QuadRef> &indices, uint32_t vertex_offset) const {
            auto translucent_vertices = vertices_.subspan(vertex_offset);
            for (auto i = 0; i < indices.size(); ++i) {
                std::memcpy(&translucent_vertices[i * 4], &vertices_in[indices[i].vertex_offset], kVertexSize * 4);
            }
        }
    };
}
