/* ==================================================================
 * pastetest.c - クリップボードのファイルを、エクスプローラーの「貼り付け」と同じ方法で貼り付ける(検証用)
 *
 *     pastetest.exe <貼り付け先のフォルダ>
 *
 *  OleGetClipboard で今のクリップボードのデータを取り、貼り付け先フォルダのドロップの受け口
 *  (IShellItem::BindToHandler(BHID_SFUIObject) の IDropTarget)へ「コピー」として落とす。
 *  エクスプローラーの貼り付けと同じく、CF_HDROP なら元のファイルから、ファイルの記述
 *  (FileGroupDescriptorW)なら FileContents の IStream を読んでコピーする。終了コード 0 = 落とせた。
 *  (PowerShell の Shell.Application の InvokeVerb("Paste") は、この PC では何も貼り付けられなかった。
 *   IFileOperation::CopyItems にデータを渡すと DV_E_FORMATETC。2026-10-05)
 * ================================================================== */

#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    IDataObject *d = NULL;
    IShellItem  *dest = NULL;
    IDropTarget *dt = NULL;
    HRESULT      hr;
    POINTL       pt = { 0, 0 };
    DWORD        eff = DROPEFFECT_COPY;

    if (argc < 2) { fwprintf(stderr, L"使い方: pastetest <貼り付け先>\n"); return 2; }
    hr = OleInitialize(NULL);
    if (SUCCEEDED(hr)) hr = OleGetClipboard(&d);
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(argv[1], NULL, &IID_IShellItem, (void **)&dest);
    if (SUCCEEDED(hr)) hr = IShellItem_BindToHandler(dest, NULL, &BHID_SFUIObject, &IID_IDropTarget, (void **)&dt);
    if (SUCCEEDED(hr)) hr = IDropTarget_DragEnter(dt, d, MK_LBUTTON, pt, &eff);
    if (SUCCEEDED(hr) && !(eff & DROPEFFECT_COPY)) { IDropTarget_DragLeave(dt); hr = E_FAIL; }
    if (SUCCEEDED(hr)) { eff = DROPEFFECT_COPY; hr = IDropTarget_Drop(dt, d, MK_LBUTTON, pt, &eff); }
    wprintf(L"pastetest: 0x%08lX 効果 %lu\n", (unsigned long)hr, eff);
    if (dt) IDropTarget_Release(dt);
    if (dest) IShellItem_Release(dest);
    if (d) IDataObject_Release(d);
    OleUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
