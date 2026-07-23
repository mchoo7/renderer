# Renderer with Multiple Backends

This project aims to implement a simple model renderer that produces consistent results across different graphics API
backends.
The sole goal of this project is to "taste" different graphics APIs. As a result, I won't implement advanced features
like ray tracing or physics based rendering.
Note that some techniques are only available on specific APIs — for example, ray tracing is limited to Vulkan, DX12,
and Metal.

Supported graphics APIs:

- OpenGL 4.6 (with Direct State Access)
- DirectX 11
- DirectX 12
- Metal 4 (using metal-cpp)
- Vulkan 1.3

Only the latest version of each API is supported in order to take full advantage of their capabilities.
