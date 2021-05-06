#include <codecvt>

#include <d3dcompiler.h>
#include <directxmath.h>

#include "sunshine/main.h"
#include "display.h"

namespace platf {
using namespace std::literals;
}

namespace platf::dxgi {
constexpr float aquamarine[] { 0.498039246f, 1.000000000f, 0.831372619f, 1.000000000f };

using input_layout_t        = util::safe_ptr<ID3D11InputLayout, Release<ID3D11InputLayout>>;
using render_target_t       = util::safe_ptr<ID3D11RenderTargetView, Release<ID3D11RenderTargetView>>;
using shader_res_t          = util::safe_ptr<ID3D11ShaderResourceView, Release<ID3D11ShaderResourceView>>;
using buf_t                 = util::safe_ptr<ID3D11Buffer, Release<ID3D11Buffer>>;
using blend_t               = util::safe_ptr<ID3D11BlendState, Release<ID3D11BlendState>>;
using raster_state_t        = util::safe_ptr<ID3D11RasterizerState, Release<ID3D11RasterizerState>>;
using sampler_state_t       = util::safe_ptr<ID3D11SamplerState, Release<ID3D11SamplerState>>;
using vs_t                  = util::safe_ptr<ID3D11VertexShader, Release<ID3D11VertexShader>>;
using ps_t                  = util::safe_ptr<ID3D11PixelShader, Release<ID3D11PixelShader>>;
using blob_t                = util::safe_ptr<ID3DBlob, Release<ID3DBlob>>;
using depth_stencil_state_t = util::safe_ptr<ID3D11DepthStencilState, Release<ID3D11DepthStencilState>>;
using depth_stencil_view_t  = util::safe_ptr<ID3D11DepthStencilView, Release<ID3D11DepthStencilView>>;

struct __attribute__ ((__aligned__ (16))) color_t {
  DirectX::XMFLOAT4 color_vec_y;
  DirectX::XMFLOAT4 color_vec_u;
  DirectX::XMFLOAT4 color_vec_v;
};

color_t make_color_matrix(float Cr, float Cb, float U_max, float V_max, float add_Y, float add_UV) {
  float Cg = 1.0f - Cr - Cb;

  float Cr_i = 1.0f - Cr;
  float Cb_i = 1.0f - Cb;

  return {
    { Cr, Cg, Cb, add_Y },
    { -(Cr * U_max / Cb_i), -(Cg * U_max / Cb_i), U_max, add_UV },
    { V_max, -(Cg * V_max / Cr_i), -(Cb * V_max / Cr_i), add_UV }
  };
}

color_t colors[] {
  make_color_matrix(0.299f, 0.114f, 0.436f, 0.615f, 0.0625, 0.5f),   // BT601 MPEG
  make_color_matrix(0.299f, 0.114f, 0.5f, 0.5f, 0.0f, 0.5f),         // BT601 JPEG
  make_color_matrix(0.2126f, 0.0722f, 0.436f, 0.615f, 0.0625, 0.5f), //BT701 MPEG
  make_color_matrix(0.2126f, 0.0722f, 0.5f, 0.5f, 0.0f, 0.5f),       //BT701 JPEG
};

template<class T>
buf_t make_buffer(device_t::pointer device, const T& t) {
  static_assert(sizeof(T) % 16 == 0, "Buffer needs to be aligned on a 16-byte alignment");

  D3D11_BUFFER_DESC buffer_desc {
    sizeof(T),
    D3D11_USAGE_IMMUTABLE,
    D3D11_BIND_CONSTANT_BUFFER
  };

  D3D11_SUBRESOURCE_DATA init_data {
    &t
  };

  buf_t::pointer buf_p;
  auto status = device->CreateBuffer(&buffer_desc, &init_data, &buf_p);
  if(status) {
    BOOST_LOG(error) << "Failed to create buffer: [0x"sv << util::hex(status).to_string_view() << ']';
    return nullptr;
  }

  return buf_t { buf_p };
}

blend_t make_blend(device_t::pointer device, bool enable) {
  D3D11_BLEND_DESC bdesc {};
  auto &rt = bdesc.RenderTarget[0];
  rt.BlendEnable = enable;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

  if(enable) {
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;

    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
  }

  blend_t::pointer blend_p;
  auto status = device->CreateBlendState(&bdesc, &blend_p);
  if(status) {
    BOOST_LOG(error) << "Failed to create blend state: [0x"sv << util::hex(status).to_string_view() << ']';
    return nullptr;
  }

  return blend_t { blend_p };
}

blob_t merge_UV_vs_hlsl;
blob_t merge_UV_ps_hlsl;
blob_t merge_Y_vs_hlsl;
blob_t merge_Y_ps_hlsl;
blob_t scene_ps_hlsl;

struct img_d3d_t : public platf::img_t {
  shader_res_t input_res;
  texture2d_t texture;
  std::shared_ptr<platf::display_t> display;

  ~img_d3d_t() override = default;
};

util::buffer_t<std::uint8_t> make_cursor_image(util::buffer_t<std::uint8_t> &&img_data, DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info)  {
  constexpr std::uint32_t black = 0xFF000000;
  constexpr std::uint32_t white = 0xFFFFFFFF;
  constexpr std::uint32_t transparent = 0;

  switch(shape_info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      std::for_each((std::uint32_t*)std::begin(img_data), (std::uint32_t*)std::end(img_data), [](auto &pixel) {
        if(pixel & 0xFF000000) {
          pixel = transparent;
        }
      });
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      return std::move(img_data);
    default:
      break;
  }

  shape_info.Height /= 2;

  util::buffer_t<std::uint8_t> cursor_img { shape_info.Width * shape_info.Height * 4 };

  auto bytes = shape_info.Pitch * shape_info.Height;
  auto pixel_begin = (std::uint32_t*)std::begin(cursor_img);
  auto pixel_data = pixel_begin;
  auto and_mask = std::begin(img_data);
  auto xor_mask = std::begin(img_data) + bytes;

  for(auto x = 0; x < bytes; ++x)  {
    for(auto c = 7; c >= 0; --c) {
      auto bit = 1 << c;
      auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);

      switch(color_type) {
        case 0: //black
          *pixel_data = black;
          break;
        case 2: //white
          *pixel_data = white;
          break;
        case 1: //transparent
        {
          *pixel_data = transparent;

          break;
        }
        case 3: //inverse
        {
          auto top_p    = pixel_data - shape_info.Width;
          auto left_p   = pixel_data - 1;
          auto right_p  = pixel_data + 1;
          auto bottom_p = pixel_data + shape_info.Width;

          // Get the x coordinate of the pixel
          auto column = (pixel_data - pixel_begin) % shape_info.Width != 0;

          if(top_p >= pixel_begin && *top_p == transparent) {
            *top_p = black;
          }

          if(column != 0 && left_p >= pixel_begin && *left_p == transparent) {
            *left_p = black;
          }

          if(bottom_p < (std::uint32_t*)std::end(cursor_img)) {
            *bottom_p = black;
          }

          if(column != shape_info.Width -1) {
            *right_p = black;
          }
          *pixel_data = white;
        }
      }

      ++pixel_data;
    }
    ++and_mask;
    ++xor_mask;
  }

  return cursor_img;
}

blob_t compile_shader(LPCSTR file, LPCSTR entrypoint, LPCSTR shader_model) {
  blob_t::pointer msg_p = nullptr;
  blob_t::pointer compiled_p;

  DWORD flags = D3DCOMPILE_ENABLE_STRICTNESS;

#ifndef NDEBUG
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> converter;

  auto wFile = converter.from_bytes(file);
  auto status = D3DCompileFromFile(wFile.c_str(), nullptr, nullptr, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);

  if(msg_p) {
    BOOST_LOG(warning) << std::string_view { (const char *)msg_p->GetBufferPointer(), msg_p->GetBufferSize() - 1 };
    msg_p->Release();
  }

  if(status) {
    BOOST_LOG(error) << "Couldn't compile ["sv << file << "] [0x"sv << util::hex(status).to_string_view() << ']';
    return nullptr;
  }

  return blob_t { compiled_p };
}

blob_t compile_pixel_shader(LPCSTR file) {
  return compile_shader(file, "main_ps", "ps_5_0");
}

blob_t compile_vertex_shader(LPCSTR file) {
  return compile_shader(file, "main_vs", "vs_5_0");
}

class hwdevice_t : public platf::hwdevice_t {
public:
  hwdevice_t(std::vector<hwdevice_t*> *hwdevices_p) : hwdevices_p { hwdevices_p } {}
  hwdevice_t() = delete;

  void set_cursor_pos(LONG rel_x, LONG rel_y, bool visible) {
    cursor_visible = visible;

    if(!visible) {
      return;
    }

    auto x = ((float)rel_x) * cursor_scale;
    auto y = ((float)rel_y) * cursor_scale;

    cursor_view.TopLeftX = x;
    cursor_view.TopLeftY = y;
    cursor_view.Width = cursor_scaled_width;
    cursor_view.Height = cursor_scaled_height;
  }

  int set_cursor_texture(texture2d_t::pointer texture, LONG width, LONG height) {
    auto device = (device_t::pointer)data;

    cursor_scaled_width = ((float)width) * cursor_scale;
    cursor_scaled_height = ((float)height) * cursor_scale;

    D3D11_SHADER_RESOURCE_VIEW_DESC desc {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_SRV_DIMENSION_TEXTURE2D
      };
      desc.Texture2D.MipLevels = 1;

    shader_res_t::pointer cursor_res_p;
    auto status = device->CreateShaderResourceView(texture, &desc, &cursor_res_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    img.input_res.reset(cursor_res_p);

    return 0;
  }

  int convert(platf::img_t &img_base) override {
    auto &img = (img_d3d_t&)img_base;

    if(!img.input_res) {
      auto device = (device_t::pointer)data;

      D3D11_SHADER_RESOURCE_VIEW_DESC desc {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        D3D11_SRV_DIMENSION_TEXTURE2D
      };
      desc.Texture2D.MipLevels = 1;

      shader_res_t::pointer input_res_p;
      auto status = device->CreateShaderResourceView(img.texture.get(), &desc, &input_res_p);
      if(FAILED(status)) {
        BOOST_LOG(error) << "Failed to create input shader resource view [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      img.input_res.reset(input_res_p);
    }

    auto input_res_p = img.input_res.get();
    auto cursor_res_p = this->img.input_res.get();

    auto scene_rt_p = scene_rt.get();
    auto Y_rt_p = nv12_Y_rt.get();
    auto UV_rt_p = nv12_UV_rt.get();

    if(cursor_visible) {
      _init_view_port(img.width, img.height);

      device_ctx_p->OMSetRenderTargets(1, &scene_rt_p, nullptr);
      device_ctx_p->VSSetShader(merge_Y_vs.get(), nullptr, 0);
      device_ctx_p->PSSetShader(scene_ps.get(), nullptr, 0);
      device_ctx_p->PSSetShaderResources(0, 1, &input_res_p);

      device_ctx_p->Draw(3, 0);

      device_ctx_p->OMSetBlendState(blend_enable.get(), nullptr, 0xFFFFFFFFu);
      device_ctx_p->RSSetViewports(1, &cursor_view);
      device_ctx_p->PSSetShaderResources(0, 1, &cursor_res_p);
      device_ctx_p->Draw(3, 0);
      device_ctx_p->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);

      input_res_p = scene_sr.get();
    }

    _init_view_port(out_width, out_height);
    device_ctx_p->OMSetRenderTargets(1, &Y_rt_p, nullptr);
    device_ctx_p->VSSetShader(merge_Y_vs.get(), nullptr, 0);
    device_ctx_p->PSSetShader(merge_Y_ps.get(), nullptr, 0);
    device_ctx_p->PSSetShaderResources(0, 1, &input_res_p);
    device_ctx_p->Draw(3, 0);

    _init_view_port(out_width / 2, out_height / 2);
    device_ctx_p->OMSetRenderTargets(1, &UV_rt_p, nullptr);
    device_ctx_p->VSSetShader(merge_UV_vs.get(), nullptr, 0);
    device_ctx_p->PSSetShader(merge_UV_ps.get(), nullptr, 0);
    device_ctx_p->PSSetShaderResources(0, 1, &input_res_p);
    device_ctx_p->Draw(3, 0);

    return 0;
  }

  void set_colorspace(std::uint32_t colorspace, std::uint32_t color_range) override {
    switch (colorspace) {
      case 5: // SWS_CS_SMPTE170M
        color_p = &colors[0];
        break;
      case 1: // SWS_CS_ITU709
        color_p = &colors[2];
        break;
      case 9: // SWS_CS_BT2020
      default:
        BOOST_LOG(warning) << "Colorspace: ["sv << colorspace << "] not yet supported: switching to default"sv;
        color_p = &colors[0];
    };

    if(color_range > 1) {
      // Full range
      ++color_p;
    }

    auto color_matrix = make_buffer((device_t::pointer)data, *color_p);
    if(!color_matrix) {
      BOOST_LOG(warning) << "Failed to create color matrix"sv;
      return;
    }

    auto buf_p = color_matrix.get();
    device_ctx_p->PSSetConstantBuffers(0, 1, &buf_p);
    this->color_matrix = std::move(color_matrix);
  }

  int init(
    std::shared_ptr<platf::display_t> display, device_t::pointer device_p, device_ctx_t::pointer device_ctx_p,
    int in_width, int in_height, int out_width, int out_height,
    pix_fmt_e pix_fmt
  ) {
    HRESULT status;

    device_p->AddRef();
    data = device_p;

    this->device_ctx_p = device_ctx_p;

    cursor_scale = (float)out_width / (float)in_width;
    cursor_visible = false;
    cursor_view.MinDepth = 0.0f;
    cursor_view.MaxDepth = 1.0f;

    platf::hwdevice_t::img = &img;

    this->out_width  = out_width;
    this->out_height = out_height;

    vs_t::pointer vs_p;
    status = device_p->CreateVertexShader(merge_Y_vs_hlsl->GetBufferPointer(), merge_Y_vs_hlsl->GetBufferSize(), nullptr, &vs_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create mergeY vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    merge_Y_vs.reset(vs_p);

    ps_t::pointer ps_p;
    status = device_p->CreatePixelShader(merge_Y_ps_hlsl->GetBufferPointer(), merge_Y_ps_hlsl->GetBufferSize(), nullptr, &ps_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create mergeY pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    merge_Y_ps.reset(ps_p);

    status = device_p->CreatePixelShader(merge_UV_ps_hlsl->GetBufferPointer(), merge_UV_ps_hlsl->GetBufferSize(), nullptr, &ps_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create mergeUV pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    merge_UV_ps.reset(ps_p);

    status = device_p->CreateVertexShader(merge_UV_vs_hlsl->GetBufferPointer(), merge_UV_vs_hlsl->GetBufferSize(), nullptr, &vs_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create mergeUV vertex shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    merge_UV_vs.reset(vs_p);

    status = device_p->CreatePixelShader(scene_ps_hlsl->GetBufferPointer(), scene_ps_hlsl->GetBufferSize(), nullptr, &ps_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create scene pixel shader [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    scene_ps.reset(ps_p);

    blend_disable = make_blend(device_p, false);
    blend_enable = make_blend(device_p, true);

    if(_init_rt(scene_sr, scene_rt, in_width, in_height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
      return -1;
    }

    color_matrix = make_buffer(device_p, colors[0]);
    if(!color_matrix) {
      BOOST_LOG(error) << "Failed to create color matrix buffer"sv;
      return -1;
    }

    float info_in[16 / sizeof(float)] { 1.0f / (float)out_width }; //aligned to 16-byte
    info_scene = make_buffer(device_p, info_in);
    if(!info_in) {
      BOOST_LOG(error) << "Failed to create info scene buffer"sv;
      return -1;
    }

    D3D11_INPUT_ELEMENT_DESC layout_desc {
      "SV_Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
    };

    input_layout_t::pointer input_layout_p;
    status = device_p->CreateInputLayout(
      &layout_desc, 1,
      merge_UV_vs_hlsl->GetBufferPointer(), merge_UV_vs_hlsl->GetBufferSize(),
      &input_layout_p);
    input_layout.reset(input_layout_p);

    D3D11_TEXTURE2D_DESC t {};
    t.Width  = out_width;
    t.Height = out_height;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = pix_fmt == pix_fmt_e::nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;
    t.BindFlags = D3D11_BIND_RENDER_TARGET;

    dxgi::texture2d_t::pointer tex_p {};
    status = device_p->CreateTexture2D(&t, nullptr, &tex_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    img.texture.reset(tex_p);
    img.display = std::move(display);
    img.width = out_width;
    img.height = out_height;
    img.data = (std::uint8_t*)tex_p;
    img.row_pitch = out_width;
    img.pixel_pitch = 1;

    D3D11_RENDER_TARGET_VIEW_DESC nv12_rt_desc {
      DXGI_FORMAT_R8_UNORM,
      D3D11_RTV_DIMENSION_TEXTURE2D
    };

    render_target_t::pointer nv12_rt_p;
    status = device_p->CreateRenderTargetView(img.texture.get(), &nv12_rt_desc, &nv12_rt_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    nv12_Y_rt.reset(nv12_rt_p);

    nv12_rt_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    status = device_p->CreateRenderTargetView(img.texture.get(), &nv12_rt_desc, &nv12_rt_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    nv12_UV_rt.reset(nv12_rt_p);

    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    sampler_state_t::pointer sampler_state_p;
    status = device_p->CreateSamplerState(&sampler_desc, &sampler_state_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create point sampler state [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    sampler_linear.reset(sampler_state_p);

    auto sampler_linear_p = sampler_linear.get();
    auto color_matrix_buf_p = color_matrix.get();
    auto info_buf_p = info_scene.get();
    device_ctx_p->OMSetBlendState(blend_disable.get(), nullptr, 0xFFFFFFFFu);
    device_ctx_p->PSSetSamplers(0, 1, &sampler_linear_p);
    device_ctx_p->PSSetConstantBuffers(0, 1, &color_matrix_buf_p);
    device_ctx_p->VSSetConstantBuffers(0, 1, &info_buf_p);
    device_ctx_p->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    device_ctx_p->IASetInputLayout(input_layout.get());

    return 0;
  }

  ~hwdevice_t() override {
    if(data) {
      ((ID3D11Device*)data)->Release();
    }

    auto it = std::find(std::begin(*hwdevices_p), std::end(*hwdevices_p), this);
    if(it != std::end(*hwdevices_p)) {
      hwdevices_p->erase(it);
    }
  }
private:
  void _init_view_port(float x, float y, float width, float height) {
    D3D11_VIEWPORT view {
      x, y,
      width, height,
      0.0f, 1.0f
    };

    device_ctx_p->RSSetViewports(1, &view);
  }

  void _init_view_port(float width, float height) {
    _init_view_port(0.0f, 0.0f, width, height);
  }

  int _init_rt(shader_res_t &shader_res, render_target_t &render_target, int width, int height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc {};

    desc.Width            = width;
    desc.Height           = height;
    desc.Format           = format;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.SampleDesc.Count = 1;

    auto device = (device_t::pointer)data;

    texture2d_t::pointer tex_p;
    auto status = device->CreateTexture2D(&desc, nullptr, &tex_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create render target texture for luma [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    texture2d_t tex { tex_p };


    D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_desc {
      format,
      D3D11_SRV_DIMENSION_TEXTURE2D
    };
    shader_resource_desc.Texture2D.MipLevels = 1;

    shader_res_t::pointer shader_res_p;
    device->CreateShaderResourceView(tex_p, &shader_resource_desc, &shader_res_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create render target texture for luma [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    shader_res.reset(shader_res_p);

    D3D11_RENDER_TARGET_VIEW_DESC render_target_desc {
      format,
      D3D11_RTV_DIMENSION_TEXTURE2D
    };

    render_target_t::pointer render_target_p;
    device->CreateRenderTargetView(tex_p, &render_target_desc, &render_target_p);
    if(status) {
      BOOST_LOG(error) << "Failed to create render target view [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    render_target.reset(render_target_p);

    return 0;
  }

public:
  color_t *color_p;

  blend_t blend_enable;
  blend_t blend_disable;

  buf_t info_scene;
  buf_t color_matrix;

  sampler_state_t sampler_linear;

  input_layout_t input_layout;

  render_target_t nv12_Y_rt;
  render_target_t nv12_UV_rt;

  render_target_t scene_rt;
  shader_res_t scene_sr;

  img_d3d_t img;

  vs_t merge_UV_vs;
  ps_t merge_UV_ps;
  vs_t merge_Y_vs;
  ps_t merge_Y_ps;
  ps_t scene_ps;

  D3D11_VIEWPORT cursor_view;
  float cursor_scaled_width, cursor_scaled_height;
  float cursor_scale;
  bool cursor_visible;

  float out_width, out_height;

  device_ctx_t::pointer device_ctx_p;

  // The destructor will remove itself from the list of hardware devices, this is done synchronously
  std::vector<hwdevice_t*> *hwdevices_p;
};

capture_e display_vram_t::snapshot(platf::img_t *img_base, std::chrono::milliseconds timeout, bool cursor_visible) {
  auto img = (img_d3d_t*)img_base;

  HRESULT status;

  DXGI_OUTDUPL_FRAME_INFO frame_info;

  resource_t::pointer res_p {};
  auto capture_status = dup.next_frame(frame_info, timeout, &res_p);
  resource_t res{ res_p };

  if(capture_status != capture_e::ok) {
    return capture_status;
  }

  const bool mouse_update_flag = frame_info.LastMouseUpdateTime.QuadPart != 0 || frame_info.PointerShapeBufferSize > 0;
  const bool frame_update_flag = frame_info.AccumulatedFrames != 0 || frame_info.LastPresentTime.QuadPart != 0;
  const bool update_flag = mouse_update_flag || frame_update_flag;

  if(!update_flag) {
    return capture_e::timeout;
  }

  if(frame_info.PointerShapeBufferSize > 0) {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

    util::buffer_t<std::uint8_t> img_data { frame_info.PointerShapeBufferSize };

    UINT dummy;
    status = dup.dup->GetFramePointerShape(img_data.size(), std::begin(img_data), &dummy, &shape_info);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to get new pointer shape [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    auto cursor_img = make_cursor_image(std::move(img_data), shape_info);

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    // Create texture for cursor
    D3D11_TEXTURE2D_DESC t {};
    t.Width  = shape_info.Width;
    t.Height = cursor_img.size() / data.SysMemPitch;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    dxgi::texture2d_t::pointer tex_p {};
    auto status = device->CreateTexture2D(&t, &data, &tex_p);
    if(FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::error;
    }
    texture2d_t texture { tex_p };

    for(auto *hwdevice : hwdevices) {
      if(hwdevice->set_cursor_texture(tex_p, t.Width, t.Height)) {
        return capture_e::error;
      }
    }

    cursor.texture = std::move(texture);
    cursor.width   = t.Width;
    cursor.height  = t.Height;
  }

  if(frame_info.LastMouseUpdateTime.QuadPart) {
    for(auto *hwdevice : hwdevices) {
      hwdevice->set_cursor_pos(frame_info.PointerPosition.Position.x, frame_info.PointerPosition.Position.y, frame_info.PointerPosition.Visible && cursor_visible);
    }
  }

  if(frame_update_flag) {
    texture2d_t::pointer src_p {};
    status = res->QueryInterface(IID_ID3D11Texture2D, (void **)&src_p);

    if(FAILED(status)) {
      BOOST_LOG(error) << "Couldn't query interface [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::error;
    }

    texture2d_t src { src_p };
    device_ctx->CopyResource(img->texture.get(), src.get());
  }

  return capture_e::ok;
}

std::shared_ptr<platf::img_t> display_vram_t::alloc_img() {
  auto img = std::make_shared<img_d3d_t>();

  D3D11_TEXTURE2D_DESC t {};
  t.Width  = width;
  t.Height = height;
  t.MipLevels = 1;
  t.ArraySize = 1;
  t.SampleDesc.Count = 1;
  t.Usage = D3D11_USAGE_DEFAULT;
  t.Format = format;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  dxgi::texture2d_t::pointer tex_p {};
  auto status = device->CreateTexture2D(&t, nullptr, &tex_p);
  if(FAILED(status)) {
    BOOST_LOG(error) << "Failed to create img buf texture [0x"sv << util::hex(status).to_string_view() << ']';
    return nullptr;
  }

  img->texture.reset(tex_p);
  img->data        = (std::uint8_t*)tex_p;
  img->row_pitch   = 0;
  img->pixel_pitch = 4;
  img->width       = 0;
  img->height      = 0;
  img->display     = shared_from_this();

  return img;
}

int display_vram_t::dummy_img(platf::img_t *img_base) {
  auto img = (img_d3d_t*)img_base;

  img->row_pitch = width * 4;
  auto dummy_data = std::make_unique<int[]>(width * height);
  D3D11_SUBRESOURCE_DATA data {
    dummy_data.get(),
    (UINT)img->row_pitch
  };

  D3D11_TEXTURE2D_DESC t {};
  t.Width  = width;
  t.Height = height;
  t.MipLevels = 1;
  t.ArraySize = 1;
  t.SampleDesc.Count = 1;
  t.Usage = D3D11_USAGE_DEFAULT;
  t.Format = format;
  t.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  dxgi::texture2d_t::pointer tex_p {};
  auto status = device->CreateTexture2D(&t, &data, &tex_p);
  if(FAILED(status)) {
    BOOST_LOG(error) << "Failed to create dummy texture [0x"sv << util::hex(status).to_string_view() << ']';
    return -1;
  }

  img->texture.reset(tex_p);
  img->data        = (std::uint8_t*)tex_p;
  img->height      = height;
  img->width       = width;
  img->pixel_pitch = 4;

  return 0;
}

std::shared_ptr<platf::hwdevice_t> display_vram_t::make_hwdevice(int width, int height, pix_fmt_e pix_fmt) {
  if(pix_fmt != platf::pix_fmt_e::nv12) {
    BOOST_LOG(error) << "display_vram_t doesn't support pixel format ["sv << from_pix_fmt(pix_fmt) << ']';

    return nullptr;
  }

  auto hwdevice = std::make_shared<hwdevice_t>(&hwdevices);

  auto ret = hwdevice->init(
    shared_from_this(),
    device.get(),
    device_ctx.get(),
    this->width, this->height,
    width, height,
    pix_fmt);

  if(ret) {
    return nullptr;
  }

  if(cursor.texture && hwdevice->set_cursor_texture(cursor.texture.get(), cursor.width, cursor.height)) {
    return nullptr;
  }

  hwdevices.emplace_back(hwdevice.get());

  return hwdevice;
}

int init() {
  BOOST_LOG(info) << "Compiling shaders..."sv;
  merge_Y_vs_hlsl = compile_vertex_shader(SUNSHINE_ASSETS_DIR "/MergeYVS.hlsl");
  if(!merge_Y_vs_hlsl) {
    return -1;
  }

  merge_Y_ps_hlsl = compile_pixel_shader(SUNSHINE_ASSETS_DIR "/MergeYPS.hlsl");
  if(!merge_Y_ps_hlsl) {
    return -1;
  }

  merge_UV_ps_hlsl = compile_pixel_shader(SUNSHINE_ASSETS_DIR "/MergeUVPS.hlsl");
  if(!merge_UV_ps_hlsl) {
    return -1;
  }

  merge_UV_vs_hlsl = compile_vertex_shader(SUNSHINE_ASSETS_DIR "/MergeUVVS.hlsl");
  if(!merge_UV_vs_hlsl) {
    return -1;
  }

  scene_ps_hlsl = compile_pixel_shader(SUNSHINE_ASSETS_DIR "/scenePS.hlsl");
  if(!scene_ps_hlsl) {
    return -1;
  }
  BOOST_LOG(info) << "Compiled shaders"sv;

  return 0;
}
}