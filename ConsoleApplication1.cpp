#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <format>
#include <algorithm>
#include <regex>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace fs = std::filesystem;
using namespace std;

// Извлекает номер страницы из имени файла (например, "page_0012.jpg" -> 12) для правильной численной сортировки
int extractPageNumber(const fs::path& filepath) {
    string stem = filepath.stem().string();
    regex numRegex(R"re((\d+))re");
    smatch match;
    string::const_iterator searchStart(stem.cbegin());
    int lastNum = -1;
    while (regex_search(searchStart, stem.cend(), match, numRegex)) {
        try {
            lastNum = stoi(match[1].str());
        }
        catch (...) {}
        searchStart = match.suffix().first;
    }
    return lastNum;
}

bool isSupportedImageExtension(string ext) {
    transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
        ext == ".gif" || ext == ".bmp" || ext == ".tif" ||
        ext == ".tiff" || ext == ".webp");
}

#ifdef _WIN32
// Вспомогательная функция поиска CLSID JPEG-кодировщика GDI+
bool getJpegEncoderClsid(CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    vector<BYTE> buffer(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, L"image/jpeg") == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return true;
        }
    }
    return false;
}

// Встроенный генератор PDF средствами C++ и Windows GDI+ 
bool buildPdfNativelyWin32(const vector<fs::path>& imageFiles, const string& outputPdfPath) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) return false;

    CLSID jpegClsid;
    if (!getJpegEncoderClsid(&jpegClsid)) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return false;
    }

    ofstream pdf(outputPdfPath, ios::binary);
    if (!pdf.is_open()) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return false;
    }

    const size_t numPages = imageFiles.size();
    // 1: Catalog, 2: Pages. Каждая страница добавляет 3 объекта (Page, Content, ImageXObject).
    const size_t totalObjects = 2 + numPages * 3;
    vector<size_t> offsets(totalObjects + 1, 0);

    auto writeLine = [&](const string& str) { pdf.write(str.c_str(), str.size()); };
    auto tellOffset = [&]() { return static_cast<size_t>(pdf.tellp()); };

    writeLine("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    offsets[1] = tellOffset();
    writeLine("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    vector<int> validPageObjIds;
    struct ImgData {
        int pageObjId;
        int contentObjId;
        int imgObjId;
        double width, height;
        string jpegBytes;
    };
    vector<ImgData> processedImages;

    // Читаем и конвертируем картинки в памяти
    for (size_t i = 0; i < numPages; ++i) {
        wstring wPath = imageFiles[i].wstring();
        Gdiplus::Image img(wPath.c_str());
        if (img.GetLastStatus() != Gdiplus::Ok || img.GetWidth() == 0 || img.GetHeight() == 0) continue;

        IStream* pStream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &pStream) != S_OK) continue;

        ULONG quality = 90;
        Gdiplus::EncoderParameters encParams;
        encParams.Count = 1;
        encParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
        encParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        encParams.Parameter[0].NumberOfValues = 1;
        encParams.Parameter[0].Value = &quality;

        if (img.Save(pStream, &jpegClsid, &encParams) == Gdiplus::Ok) {
            STATSTG stat = {};
            if (pStream->Stat(&stat, STATFLAG_NONAME) == S_OK && stat.cbSize.QuadPart > 0) {
                size_t jpegLen = static_cast<size_t>(stat.cbSize.QuadPart);
                string jpegBytes(jpegLen, '\0');
                LARGE_INTEGER zero = {};
                pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                ULONG bytesRead = 0;
                pStream->Read(jpegBytes.data(), static_cast<ULONG>(jpegLen), &bytesRead);

                if (bytesRead > 0) {
                    ImgData d;
                    d.width = img.GetWidth();
                    d.height = img.GetHeight();
                    d.jpegBytes = move(jpegBytes);
                    processedImages.push_back(move(d));
                }
            }
        }
        pStream->Release();
    }

    if (processedImages.empty()) {
        pdf.close();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return false;
    }

    // Записываем PDF-объекты со строгим выравниванием смещений
    for (size_t i = 0; i < processedImages.size(); ++i) {
        auto& d = processedImages[i];
        d.pageObjId = static_cast<int>(3 + i * 3);
        d.contentObjId = d.pageObjId + 1;
        d.imgObjId = d.pageObjId + 2;
        validPageObjIds.push_back(d.pageObjId);

        double pdfWidth = 595.0; // A4 (пропорции подгоняются под картинку)
        double pdfHeight = (d.height / d.width) * pdfWidth;

        offsets[d.pageObjId] = tellOffset();
        writeLine(format("{} 0 obj\n<< /Type /Page /Parent 2 0 R /Resources << /XObject << /Im0 {} 0 R >> >> /MediaBox [0 0 {:.2f} {:.2f}] /Contents {} 0 R >>\nendobj\n",
            d.pageObjId, d.imgObjId, pdfWidth, pdfHeight, d.contentObjId));

        // ИСПРАВЛЕНИЕ: Операторы графического стека разделены переносами строк
        string contentStream = format("q\n{:.2f} 0 0 {:.2f} 0 0 cm\n/Im0 Do\nQ\n", pdfWidth, pdfHeight);
        offsets[d.contentObjId] = tellOffset();
        writeLine(format("{} 0 obj\n<< /Length {} >>\nstream\n{}endstream\nendobj\n",
            d.contentObjId, contentStream.size(), contentStream));

        offsets[d.imgObjId] = tellOffset();
        writeLine(format("{} 0 obj\n<< /Type /XObject /Subtype /Image /Width {} /Height {} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length {} >>\nstream\n",
            d.imgObjId, static_cast<int>(d.width), static_cast<int>(d.height), d.jpegBytes.size()));
        pdf.write(d.jpegBytes.data(), d.jpegBytes.size());
        writeLine("\nendstream\nendobj\n");
    }

    // Объект Pages
    offsets[2] = tellOffset();
    writeLine("2 0 obj\n<< /Type /Pages /Kids [ ");
    for (int id : validPageObjIds) writeLine(format("{} 0 R ", id));
    writeLine(format("] /Count {} >>\nendobj\n", validPageObjIds.size()));

    size_t xrefOffset = tellOffset();

    // ИСПРАВЛЕНИЕ: Таблица xref требует строго 20 байт на строку вместе с CRLF
    writeLine(format("xref\n0 {}\n", processedImages.size() * 3 + 3));
    writeLine("0000000000 65535 f \r\n");

    for (size_t objId = 1; objId < processedImages.size() * 3 + 3; ++objId) {
        if (offsets[objId] > 0) {
            string line = format("{:010} 00000 n \r\n", offsets[objId]);
            pdf.write(line.c_str(), line.size());
        }
        else {
            writeLine("0000000000 65535 f \r\n");
        }
    }

    writeLine(format("trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{}\n%%EOF\n",
        processedImages.size() * 3 + 3, xrefOffset));

    pdf.close();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return true;
}
#endif

int main() {
    setlocale(LC_ALL, "Russian");
    const string inputDir = "captured_pages";
    const string outputPdf = "output_bararan_114.pdf";

    cout << "========================================================\n";
    cout << " СБОРКА PDF ИЗ ИЗОБРАЖЕНИЙ В ДИРЕКТОРИИ 'captured_pages'\n";
    cout << "========================================================\n\n";

    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) {
        cerr << format("[-] Ошибка: Директория '{}' не найдена!\n", inputDir);
        return 1;
    }

    vector<fs::path> imageFiles;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file()) {
            string ext = entry.path().extension().string();
            if (isSupportedImageExtension(ext) && entry.file_size() > 0) {
                imageFiles.push_back(entry.path());
            }
        }
    }

    if (imageFiles.empty()) {
        cerr << format("[-] В директории '{}' не найдено изображений.\n", inputDir);
        return 1;
    }

    sort(imageFiles.begin(), imageFiles.end(), [](const fs::path& a, const fs::path& b) {
        int numA = extractPageNumber(a);
        int numB = extractPageNumber(b);
        if (numA != numB && numA != -1 && numB != -1) return numA < numB;
        return a.filename().string() < b.filename().string();
        });

    cout << format("[+] Найдено и отсортировано изображений: {}\n", imageFiles.size());

#ifdef _WIN32
    cout << "[*] Сборка PDF напрямую средствами C++ (GDI+)...\n";
    if (buildPdfNativelyWin32(imageFiles, outputPdf)) {
        cout << format("\n[+] УСПЕХ! PDF-файл '{}' успешно создан.\n", outputPdf);
        return 0;
    }
#endif

    // Резервный запуск ImageMagick (если GDI+ недоступен или произошла ошибка)
    string listFilePath = inputDir + "/filelist.txt";
    ofstream listFile(listFilePath);
    for (const auto& imgPath : imageFiles) listFile << "\"" << imgPath.generic_string() << "\"\n";
    listFile.close();

    cout << "[*] Компиляция PDF через ImageMagick...\n";
    string pdfCommand = format("magick @\"{}\" \"{}\"", listFilePath, outputPdf);
    int result = system(pdfCommand.c_str());
    fs::remove(listFilePath);

    if (result == 0) cout << format("\n[+] УСПЕХ! PDF-файл '{}' сохранен.\n", outputPdf);
    else cerr << "[-] Ошибка ImageMagick.\n";
    return result;
}