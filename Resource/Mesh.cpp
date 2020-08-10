#include "Mesh.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "../Device/CommandGraph.h"
#include "../Core/Utils.h"

using namespace FrameDX12;

void Mesh::BuildFromOBJ(Device* device, CommandGraph& copy_graph, const std::string& path)
{
    using namespace std;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    std::string warn;
    std::string err;

    bool succeeded = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path.c_str());

    if (!warn.empty()) LogMsg(StringToWString(warn), LogCategory::Warning);
    if (!err.empty()) LogMsg(StringToWString(err), LogCategory::Error);

    if (!succeeded)
        return;

    // Create CPU-side vertex and index buffers
    unordered_map<StandardVertex, uint32_t> unique_vertices = {};

    size_t reserve_size = 0;
    for (const auto& shape : shapes)
        reserve_size += shape.mesh.indices.size() * 3;
    mVertices.reserve(reserve_size);
    mIndices.reserve(reserve_size);

    for (const auto& shape : shapes)
    {
        for (const auto& index : shape.mesh.indices)
        {
            StandardVertex vertex;
            vertex.position.x = attrib.vertices[3 * index.vertex_index + 0];
            vertex.position.y = attrib.vertices[3 * index.vertex_index + 1];
            vertex.position.z = attrib.vertices[3 * index.vertex_index + 2];

            vertex.normal.x = attrib.normals[3 * index.normal_index + 0];
            vertex.normal.y = attrib.normals[3 * index.normal_index + 1];
            vertex.normal.z = attrib.normals[3 * index.normal_index + 2];

            vertex.uv.x = attrib.texcoords[2 * index.texcoord_index + 0];
            vertex.uv.y = attrib.texcoords[2 * index.texcoord_index + 1];

            if (!unique_vertices.count(vertex))
            {
                unique_vertices[vertex] = static_cast<uint32_t>(mVertices.size());
                mVertices.push_back(vertex);
            }

            mIndices.push_back(unique_vertices[vertex]);
        }
    }

    mDesc.index_count = mIndices.size();
    mDesc.vertex_count = mVertices.size();
    mDesc.triangle_count = mDesc.index_count / 3;

    // TODO : Compute tangents

    // Create GPU-side buffers
    mIndexBuffer.Create(device, CD3DX12_RESOURCE_DESC::Buffer(mIndices.size() * sizeof(uint32_t)));
    mVertexBuffer.Create(device, CD3DX12_RESOURCE_DESC::Buffer(mVertices.size() * sizeof(StandardVertex)));

    copy_graph.AddNode("", nullptr, [&](ID3D12GraphicsCommandList* cl, uint32_t)
    {
        // Need to set it to common when using the copy queue
        // TODO : It would be nice if the command graph could batch resource barriers
        mIndexBuffer.FillFromBuffer(cl, mIndices, D3D12_RESOURCE_STATE_COMMON);
        mVertexBuffer.FillFromBuffer(cl, mVertices, D3D12_RESOURCE_STATE_COMMON);
    }, {});

    mVBV.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVBV.SizeInBytes = mVertices.size() * sizeof(StandardVertex);
    mVBV.StrideInBytes = sizeof(StandardVertex);

    mIBV.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIBV.SizeInBytes = mIndices.size() * sizeof(uint32_t);
    mIBV.Format = DXGI_FORMAT_R32_UINT;
}

void Mesh::Draw(ID3D12GraphicsCommandList* cl)
{
    // Remember, the transitions do nothing (that is, this function, not DX barriers) if the resource is already on the state you want
    mVertexBuffer.Transition(cl, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    mIndexBuffer.Transition(cl, D3D12_RESOURCE_STATE_INDEX_BUFFER);

    cl->IASetIndexBuffer(&mIBV);
    cl->IASetVertexBuffers(0, 1, &mVBV);
    cl->DrawIndexedInstanced(mIndices.size(), 1, 0, 0, 0);
}