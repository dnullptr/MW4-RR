#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <fstream>
#include <vector>
#include <cstdio>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr,const char* what){if(FAILED(hr)){printf("FAIL: %s HRESULT=%08lx\n",what,hr);exit(1);}}
int wmain(int argc,wchar_t** argv){
 if(argc!=4){puts("usage: rr-dxil-assemble dxcompiler.dll input.ll output.cso");return 1;}
 HMODULE module=LoadLibraryW(argv[1]);if(!module){puts("FAIL: load Microsoft DXC");return 1;}auto create=reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module,"DxcCreateInstance"));if(!create)return 1;
 ComPtr<IDxcLibrary> library;ComPtr<IDxcAssembler> assembler;ComPtr<IDxcValidator> validator;check(create(CLSID_DxcLibrary,IID_PPV_ARGS(&library)),"DXC library");check(create(CLSID_DxcAssembler,IID_PPV_ARGS(&assembler)),"DXIL assembler");check(create(CLSID_DxcValidator,IID_PPV_ARGS(&validator)),"DXIL validator");
 std::ifstream input(argv[2],std::ios::binary);if(!input)return 1;std::vector<char> text{std::istreambuf_iterator<char>(input),{}};ComPtr<IDxcBlobEncoding> source;check(library->CreateBlobWithEncodingOnHeapCopy(text.data(),UINT32(text.size()),CP_UTF8,&source),"source blob");
 auto result=[&](IDxcOperationResult* operation){HRESULT status{};check(operation->GetStatus(&status),"operation status");if(FAILED(status)){ComPtr<IDxcBlobEncoding> errors;operation->GetErrorBuffer(&errors);if(errors)printf("%.*s\n",int(errors->GetBufferSize()),static_cast<const char*>(errors->GetBufferPointer()));check(status,"DXIL operation");}};
 ComPtr<IDxcOperationResult> assembled;check(assembler->AssembleToContainer(source.Get(),&assembled),"assemble");result(assembled.Get());ComPtr<IDxcBlob> code;check(assembled->GetResult(&code),"assembled container");
 ComPtr<IDxcOperationResult> validated;check(validator->Validate(code.Get(),DxcValidatorFlags_InPlaceEdit,&validated),"validate and sign");result(validated.Get());
 std::ofstream output(argv[3],std::ios::binary);output.write(static_cast<const char*>(code->GetBufferPointer()),code->GetBufferSize());output.close();if(!output)return 1;printf("Validated DXIL container: %zu bytes\n",code->GetBufferSize());
}
