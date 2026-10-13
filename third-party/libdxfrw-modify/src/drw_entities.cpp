/******************************************************************************
**  libDXFrw - Library to read/write DXF files (ascii & binary)              **
**                                                                           **
**  Copyright (C) 2011-2015 José F. Soriano, rallazz@gmail.com               **
**                                                                           **
**  This library is free software, licensed under the terms of the GNU       **
**  General Public License as published by the Free Software Foundation,     **
**  either version 2 of the License, or (at your option) any later version.  **
**  You should have received a copy of the GNU General Public License        **
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.    **
******************************************************************************/

#include "drw_entities.h"
#include "intern/dxfreader.h"
#include "intern/dwgbuffer.h"
#include "intern/drw_dbg.h"


//! Calculate arbitary axis
/*!
*   Calculate arbitary axis for apply extrusions
*  @author Rallaz
*/
void DRW_Entity::calculateAxis(DRW_Coord extPoint){
    //Follow the arbitrary DXF definitions for extrusion axes.
    if (fabs(extPoint.x) < 0.015625 && fabs(extPoint.y) < 0.015625) {
        //If we get here, implement Ax = Wy x N where Wy is [0,1,0] per the DXF spec.
        //The cross product works out to Wy.y*N.z-Wy.z*N.y, Wy.z*N.x-Wy.x*N.z, Wy.x*N.y-Wy.y*N.x
        //Factoring in the fixed values for Wy gives N.z,0,-N.x
        extAxisX.x = extPoint.z;
        extAxisX.y = 0;
        extAxisX.z = -extPoint.x;
    } else {
        //Otherwise, implement Ax = Wz x N where Wz is [0,0,1] per the DXF spec.
        //The cross product works out to Wz.y*N.z-Wz.z*N.y, Wz.z*N.x-Wz.x*N.z, Wz.x*N.y-Wz.y*N.x
        //Factoring in the fixed values for Wz gives -N.y,N.x,0.
        extAxisX.x = -extPoint.y;
        extAxisX.y = extPoint.x;
        extAxisX.z = 0;
    }

    extAxisX.unitize();

    //Ay = N x Ax
    extAxisY.x = (extPoint.y * extAxisX.z) - (extAxisX.y * extPoint.z);
    extAxisY.y = (extPoint.z * extAxisX.x) - (extAxisX.z * extPoint.x);
    extAxisY.z = (extPoint.x * extAxisX.y) - (extAxisX.x * extPoint.y);

    extAxisY.unitize();
}

//! Extrude a point using arbitary axis
/*!
*   apply extrusion in a point using arbitary axis (previous calculated)
*  @author Rallaz
*/
void DRW_Entity::extrudePoint(DRW_Coord extPoint, DRW_Coord *point){
    double px, py, pz;
    px = (extAxisX.x*point->x)+(extAxisY.x*point->y)+(extPoint.x*point->z);
    py = (extAxisX.y*point->x)+(extAxisY.y*point->y)+(extPoint.y*point->z);
    pz = (extAxisX.z*point->x)+(extAxisY.z*point->y)+(extPoint.z*point->z);

    point->x = px;
    point->y = py;
    point->z = pz;
}

bool DRW_Entity::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 5:
        handle = reader->getHandleString();
        break;
    case 330:
        parentHandle = reader->getHandleString();
        break;
    case 8:
        layer = reader->getUtf8String();
        break;
    case 6:
        lineType = reader->getUtf8String();
        break;
    case 62:
        color = reader->getInt32();
        break;
    case 370:
        lWeight = DRW_LW_Conv::dxfInt2lineWidth(reader->getInt32());
        break;
    case 48:
        ltypeScale = reader->getDouble();
        break;
    case 60:
        // DXF 规范中 60=1 表示**不可见**，因此必须取反，
        // 否则隐藏图元会被读成可见。
        visible = !reader->getBool();
        break;
    case 420:
        color24 = reader->getInt32();
        break;
    case 430:
        colorName = reader->getString();
        break;
    case 67:
        space = static_cast<DRW::Space>(reader->getInt32());
        break;
    case 102:
        parseDxfGroups(code, reader);
        break;
    case 1000:
    case 1001:
    case 1002:
    case 1003:
    case 1004:
    case 1005:
        extData.push_back(new DRW_Variant(code, reader->getString()));
        break;
    case 1010:
    case 1011:
    case 1012:
    case 1013:
        curr = new DRW_Variant(code, DRW_Coord(reader->getDouble(), 0.0, 0.0));
        extData.push_back(curr);
        break;
    case 1020:
    case 1021:
    case 1022:
    case 1023:
        if (curr)
            curr->setCoordY(reader->getDouble());
        break;
    case 1030:
    case 1031:
    case 1032:
    case 1033:
        if (curr)
            curr->setCoordZ(reader->getDouble());
        curr=NULL;
        break;
    case 1040:
    case 1041:
    case 1042:
        extData.push_back(new DRW_Variant(code, reader->getDouble() ));
        break;
    case 1070:
    case 1071:
        extData.push_back(new DRW_Variant(code, reader->getInt32() ));
        break;
    default:
        break;
    }
    return true;
}

//parses dxf 102 groups to read entity
bool DRW_Entity::parseDxfGroups(int code, dxfReader *reader){
    std::list<DRW_Variant> ls;
    DRW_Variant curr;
    int nc;
    std::string appName= reader->getString();
    if (!appName.empty() && appName.at(0)== '{'){
        curr.addString(code, appName.substr(1, (int) appName.size()-1));
        ls.push_back(curr);
        while (code !=102 && appName.at(0)== '}'){
            reader->readRec(&nc);//RLZ curr.code = code or nc?
//            curr.code = code;
            //RLZ code == 330 || code == 360 OR nc == 330 || nc == 360 ?
            if (code == 330 || code == 360)
                curr.addInt(code, reader->getHandleString());//RLZ code or nc
            else {
                switch (reader->type) {
                case dxfReader::STRING:
                    curr.addString(code, reader->getString());//RLZ code or nc
                    break;
                case dxfReader::INT32:
                case dxfReader::INT64:
                    curr.addInt(code, reader->getInt32());//RLZ code or nc
                    break;
                case dxfReader::DOUBLE:
                    curr.addDouble(code, reader->getDouble());//RLZ code or nc
                    break;
                case dxfReader::BOOL:
                    curr.addInt(code, reader->getInt32());//RLZ code or nc
                    break;
                default:
                    break;
                }
            }
            ls.push_back(curr);
        }
    }

    appData.push_back(ls);
    return true;
}

bool DRW_Entity::parseDwg(DRW::Version version, dwgBuffer *buf, dwgBuffer* strBuf, duint32 bs){
    objSize=0;
    DRW_DBG("\n***************************** parsing entity *********************************************\n");
    oType = buf->getObjType(version);
    DRW_DBG("Object type: "); DRW_DBG(oType); DRW_DBG(", "); DRW_DBGH(oType);

    if (version > DRW::AC1014 && version < DRW::AC1024) {//2000 & 2004
        objSize = buf->getRawLong32();  //RL 32bits object size in bits
        DRW_DBG(" Object size: "); DRW_DBG(objSize); DRW_DBG("\n");
    }
    if (version > DRW::AC1021) {//2010+
        duint32 ms = buf->size();
        objSize = ms*8 - bs;
        DRW_DBG(" Object size: "); DRW_DBG(objSize); DRW_DBG("\n");
    }

    if (strBuf != NULL && version > DRW::AC1018) {//2007+
        strBuf->moveBitPos(objSize-1);
        DRW_DBG(" strBuf strbit pos 2007: "); DRW_DBG(strBuf->getPosition()); DRW_DBG(" strBuf bpos 2007: "); DRW_DBG(strBuf->getBitPos()); DRW_DBG("\n");
        if (strBuf->getBit() == 1){
            DRW_DBG("DRW_TableEntry::parseDwg string bit is 1\n");
            strBuf->moveBitPos(-17);
            duint16 strDataSize = strBuf->getRawShort16();
            DRW_DBG("\nDRW_TableEntry::parseDwg string strDataSize: "); DRW_DBGH(strDataSize); DRW_DBG("\n");
            if ( (strDataSize& 0x8000) == 0x8000){
                DRW_DBG("\nDRW_TableEntry::parseDwg string 0x8000 bit is set");
                strBuf->moveBitPos(-33);//RLZ pending to verify
                duint16 hiSize = strBuf->getRawShort16();
                strDataSize = ((strDataSize&0x7fff) | (hiSize<<15));
            }
            strBuf->moveBitPos( -strDataSize -16); //-14
            DRW_DBG("strBuf start strDataSize pos 2007: "); DRW_DBG(strBuf->getPosition()); DRW_DBG(" strBuf bpos 2007: "); DRW_DBG(strBuf->getBitPos()); DRW_DBG("\n");
        } else
            DRW_DBG("\nDRW_TableEntry::parseDwg string bit is 0");
        DRW_DBG("strBuf start pos 2007: "); DRW_DBG(strBuf->getPosition()); DRW_DBG(" strBuf bpos 2007: "); DRW_DBG(strBuf->getBitPos()); DRW_DBG("\n");
    }

    dwgHandle ho = buf->getHandle();
    handle = ho.ref;
    DRW_DBG("Entity Handle: "); DRW_DBGHL(ho.code, ho.size, ho.ref);
    dint16 extDataSize = buf->getBitShort(); //BS
    DRW_DBG(" ext data size: "); DRW_DBG(extDataSize);
    while (extDataSize>0 && buf->isGood()) {
        /* RLZ: TODO */
        dwgHandle ah = buf->getHandle();
        DRW_DBG("App Handle: "); DRW_DBGHL(ah.code, ah.size, ah.ref);
        duint8 *tmpExtData = new duint8[extDataSize];
        buf->getBytes(tmpExtData, extDataSize);
        dwgBuffer tmpExtDataBuf(tmpExtData, extDataSize, buf->decoder);

        duint8 dxfCode = tmpExtDataBuf.getRawChar8();
        DRW_DBG(" dxfCode: "); DRW_DBG(dxfCode);
        switch (dxfCode){
        case 0:{
            duint8 strLength = tmpExtDataBuf.getRawChar8();
            DRW_DBG(" strLength: "); DRW_DBG(strLength);
            duint16 cp = tmpExtDataBuf.getBERawShort16();
            DRW_DBG(" str codepage: "); DRW_DBG(cp);
            for (int i=0;i< strLength+1;i++) {//string length + null terminating char
                duint8 dxfChar = tmpExtDataBuf.getRawChar8();
                DRW_DBG(" dxfChar: "); DRW_DBG(dxfChar);
            }
            break;
        }
        default:
            /* RLZ: TODO */
            break;
        }
        delete[]tmpExtData;
        extDataSize = buf->getBitShort(); //BS
        DRW_DBG(" ext data size: "); DRW_DBG(extDataSize);
    } //end parsing extData (EED)
    duint8 graphFlag = buf->getBit(); //B
    DRW_DBG(" graphFlag: "); DRW_DBG(graphFlag); DRW_DBG("\n");
    if (graphFlag) {
        // 预览/代理图形数据的长度：R13–2007 是 RL（32 位定长），**2010 起是 BLL**
        //（变长比特编码）。照 RL 读会让后面的字段整体错位、对象解析失败——
        // 只有带预览数据的对象才会踩到，而类对象（如 ARC_DIMENSION）几乎都带，
        // 症状就是「某个类对象整体读不出来」。
        duint32 graphDataSize = (version > DRW::AC1021)
                                    ? static_cast<duint32>(buf->getBitLongLong())
                                    : buf->getRawLong32();
        DRW_DBG("graphData in bytes: "); DRW_DBG(graphDataSize); DRW_DBG("\n");
// RLZ: TODO
        //skip graphData bytes
        duint8 *tmpGraphData = new duint8[graphDataSize];
        buf->getBytes(tmpGraphData, graphDataSize);
        dwgBuffer tmpGraphDataBuf(tmpGraphData, graphDataSize, buf->decoder);
        DRW_DBG("graph data remaining bytes: "); DRW_DBG(tmpGraphDataBuf.numRemainingBytes()); DRW_DBG("\n");
        delete[]tmpGraphData;
    }
    if (version < DRW::AC1015) {//14-
        objSize = buf->getRawLong32();  //RL 32bits object size in bits
        DRW_DBG(" Object size in bits: "); DRW_DBG(objSize); DRW_DBG("\n");
    }

    duint8 entmode = buf->get2Bits(); //BB
    if (entmode == 0)
        ownerHandle= true;
//        entmode = 2;
    else if(entmode ==2)
        entmode = 0;
    space = (DRW::Space)entmode; //RLZ verify cast values
    DRW_DBG("entmode: "); DRW_DBG(entmode);
    numReactors = buf->getBitShort(); //BS
    DRW_DBG(", numReactors: "); DRW_DBG(numReactors);

    if (version < DRW::AC1015) {//14-
        if(buf->getBit()) {//is bylayer line type
            lineType = "BYLAYER";
            ltFlags = 0;
        } else {
            lineType = "";
            ltFlags = 3;
        }
        DRW_DBG(" lineType: "); DRW_DBG(lineType.c_str());
        DRW_DBG(" ltFlags: "); DRW_DBG(ltFlags);
    }
    if (version > DRW::AC1015) {//2004+
        xDictFlag = buf->getBit();
        DRW_DBG(" xDictFlag: "); DRW_DBG(xDictFlag); DRW_DBG("\n");
    }

    // nolinks（prev/next 链标志）：按 libredwg common_entity_data.spec 的
    // `VERSIONS (R_13b1, R_2002)` 只在 R13~R2002 存在；R2013+ 起这个位置改为
    // has_ds_data（见下）。原先的 `version > AC1024` 会让 AC1027 多读 1 位，
    // 与 libredwg / CAD 内核的字段序列错开。
    if (version < DRW::AC1018) {
        haveNextLinks = buf->getBit(); //aka nolinks //B
        DRW_DBG(", haveNextLinks (0 yes, 1 prev next): "); DRW_DBG(haveNextLinks); DRW_DBG("\n");
    } else {
        haveNextLinks = 1; //aka nolinks //B
        DRW_DBG(", haveNextLinks (forced): "); DRW_DBG(haveNextLinks); DRW_DBG("\n");
    }
    // 2013+：has_ds_data 位（libredwg `SINCE (R_2013) FIELD_B (has_ds_data, 0)`）。
    // 为真时其后还要跟 ACIS SAB 句柄；本实现不承载 3D 实体数据，读出即丢弃。
    if (version > DRW::AC1024) {
        duint8 hasDsData = buf->getBit();
        DRW_DBG(", has_ds_data: "); DRW_DBG(hasDsData); DRW_DBG("\n");
    }
//ENC color
    color = buf->getEnColor(version); //BS or CMC //ok for R14 or negate
    ltypeScale = buf->getBitDouble(); //BD
    DRW_DBG(" entity color: "); DRW_DBG(color);
    DRW_DBG(" ltScale: "); DRW_DBG(ltypeScale); DRW_DBG("\n");
    if (version > DRW::AC1014) {//2000+
        UTF8STRING plotStyleName;
        for (duint8 i = 0; i<2;++i) { //two flags in one
            plotFlags = buf->get2Bits(); //BB
            if (plotFlags == 1)
                plotStyleName = "byblock";
            else if (plotFlags == 2)
                plotStyleName = "continuous";
            else if (plotFlags == 0)
                plotStyleName = "bylayer";
            else //handle at end
                plotStyleName = "";
            if (i == 0) {
                ltFlags = plotFlags;
                lineType = plotStyleName; //RLZ: howto solve? if needed plotStyleName;
                DRW_DBG("ltFlags: "); DRW_DBG(ltFlags);
                DRW_DBG(" lineType: "); DRW_DBG(lineType.c_str());
            } else {
                DRW_DBG(", plotFlags: "); DRW_DBG(plotFlags);
            }
        }
    }
    if (version > DRW::AC1018) {//2007+
        materialFlag = buf->get2Bits(); //BB
        DRW_DBG("materialFlag: "); DRW_DBG(materialFlag);
        shadowFlag = buf->getRawChar8(); //RC
        DRW_DBG("shadowFlag: "); DRW_DBG(shadowFlag); DRW_DBG("\n");
    }
    if (version > DRW::AC1021) {//2010+
        duint8 visualFlags = buf->get2Bits(); //full & face visual style
        DRW_DBG("shadowFlag 2: "); DRW_DBG(visualFlags); DRW_DBG("\n");
        duint8 unk = buf->getBit(); //edge visual style
        DRW_DBG("unknown bit: "); DRW_DBG(unk); DRW_DBG("\n");
    }
    dint16 invisibleFlag = buf->getBitShort(); //BS
    DRW_DBG(" invisibleFlag: "); DRW_DBG(invisibleFlag);
    // 该标志表示图元是否不可见：0 = 可见，非 0 = 不可见。
    // 必须赋给 visible，否则 DWG 里隐藏的图元会一律按可见导入。
    visible = (invisibleFlag == 0);
    if (version > DRW::AC1014) {//2000+
        lWeight = DRW_LW_Conv::dwgInt2lineWidth( buf->getRawChar8() ); //RC
        DRW_DBG(" lwFlag (lWeight): "); DRW_DBG(lWeight); DRW_DBG("\n");
    }
    //Only in blocks ????????
//    if (version > DRW::AC1018) {//2007+
//        duint8 unk = buf->getBit();
//        DRW_DBG("unknown bit: "); DRW_DBG(unk); DRW_DBG("\n");
//    }
    return buf->isGood();
}

bool DRW_Entity::parseDwgEntHandle(DRW::Version version, dwgBuffer *buf){
    if (version > DRW::AC1018) {//2007+ skip string area
        buf->setPosition(objSize >> 3);
        buf->setBitPos(objSize & 7);
    }

    if(ownerHandle){//entity are in block or in a polyline
        dwgHandle ownerH = buf->getOffsetHandle(handle);
        DRW_DBG("owner (parent) Handle: "); DRW_DBGHL(ownerH.code, ownerH.size, ownerH.ref); DRW_DBG("\n");
        DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        parentHandle = ownerH.ref;
        DRW_DBG("Block (parent) Handle: "); DRW_DBGHL(ownerH.code, ownerH.size, parentHandle); DRW_DBG("\n");
    } else
        DRW_DBG("NO Block (parent) Handle\n");

    DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    for (int i=0; i< numReactors;++i) {
        dwgHandle reactorsH = buf->getHandle();
        DRW_DBG(" reactorsH control Handle: "); DRW_DBGHL(reactorsH.code, reactorsH.size, reactorsH.ref); DRW_DBG("\n");
    }
    if (xDictFlag !=1){//linetype in 2004 seems not have XDicObjH or NULL handle
        dwgHandle XDicObjH = buf->getHandle();
        DRW_DBG(" XDicObj control Handle: "); DRW_DBGHL(XDicObjH.code, XDicObjH.size, XDicObjH.ref); DRW_DBG("\n");
    }
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    if (version < DRW::AC1015) {//R14-
        //layer handle
        layerH = buf->getOffsetHandle(handle);
        DRW_DBG(" layer Handle: "); DRW_DBGHL(layerH.code, layerH.size, layerH.ref); DRW_DBG("\n");
        DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        //lineType handle
        if(ltFlags == 3){
            lTypeH = buf->getOffsetHandle(handle);
            DRW_DBG("linetype Handle: "); DRW_DBGHL(lTypeH.code, lTypeH.size, lTypeH.ref); DRW_DBG("\n");
            DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        }
    }
    if (version < DRW::AC1018) {//2000+
        if (haveNextLinks == 0) {
            dwgHandle nextLinkH = buf->getOffsetHandle(handle);
            DRW_DBG(" prev nextLinkers Handle: "); DRW_DBGHL(nextLinkH.code, nextLinkH.size, nextLinkH.ref); DRW_DBG("\n");
            DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
            prevEntLink = nextLinkH.ref;
            nextLinkH = buf->getOffsetHandle(handle);
            DRW_DBG(" next nextLinkers Handle: "); DRW_DBGHL(nextLinkH.code, nextLinkH.size, nextLinkH.ref); DRW_DBG("\n");
            DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
            nextEntLink = nextLinkH.ref;
        } else {
            nextEntLink = handle+1;
            prevEntLink = handle-1;
        }
    }
    if (version > DRW::AC1015) {//2004+
        //Parses Bookcolor handle
    }
    if (version > DRW::AC1014) {//2000+
        //layer handle
        layerH = buf->getOffsetHandle(handle);
        DRW_DBG(" layer Handle: "); DRW_DBGHL(layerH.code, layerH.size, layerH.ref); DRW_DBG("\n");
        DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        //lineType handle
        if(ltFlags == 3){
            lTypeH = buf->getOffsetHandle(handle);
            DRW_DBG("linetype Handle: "); DRW_DBGHL(lTypeH.code, lTypeH.size, lTypeH.ref); DRW_DBG("\n");
            DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        }
    }
    if (version > DRW::AC1014) {//2000+
        if (version > DRW::AC1018) {//2007+
            if (materialFlag == 3) {
                dwgHandle materialH = buf->getOffsetHandle(handle);
                DRW_DBG(" material Handle: "); DRW_DBGHL(materialH.code, materialH.size, materialH.ref); DRW_DBG("\n");
                DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
            }
            if (shadowFlag == 3) {
                dwgHandle shadowH = buf->getOffsetHandle(handle);
                DRW_DBG(" shadow Handle: "); DRW_DBGHL(shadowH.code, shadowH.size, shadowH.ref); DRW_DBG("\n");
                DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
            }
        }
        if (plotFlags == 3) {
            dwgHandle plotStyleH = buf->getOffsetHandle(handle);
            DRW_DBG(" plot style Handle: "); DRW_DBGHL(plotStyleH.code, plotStyleH.size, plotStyleH.ref); DRW_DBG("\n");
            DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        }
    }
    DRW_DBG("\n DRW_Entity::parseDwgEntHandle Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    return buf->isGood();
}

void DRW_Point::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 10:
        basePoint.x = reader->getDouble();
        break;
    case 20:
        basePoint.y = reader->getDouble();
        break;
    case 30:
        basePoint.z = reader->getDouble();
        break;
    case 39:
        thickness = reader->getDouble();
        break;
    case 210:
        haveExtrusion = true;
        extPoint.x = reader->getDouble();
        break;
    case 220:
        extPoint.y = reader->getDouble();
        break;
    case 230:
        extPoint.z = reader->getDouble();
        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

bool DRW_Point::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing point *********************************************\n");

    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    thickness = buf->getThickness(version > DRW::AC1014);//BD
    DRW_DBG("\nthickness: "); DRW_DBG(thickness);
    extPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG(", Extrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);

    double x_axis = buf->getBitDouble();//BD
    DRW_DBG("\n  x_axis: ");DRW_DBG(x_axis);DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    //    RS crc;   //RS */

    return buf->isGood();
}

// -----------------------------------------------------------------------------
//  LIGHT（AcDbLight）
// -----------------------------------------------------------------------------
//  字段与组码的对应关系见 drw_entities.h 的类注释（取自 libredwg dwg2.spec 的
//  DWG_ENTITY (LIGHT)）。这里只做逐码赋值；未建模的光度扩展块与代理图形
//  （92 + 310）会落到 default 分支，由基类按公共组码处理或忽略。
void DRW_Light::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 90:
        classVersion = reader->getInt32();
        break;
    case 1:
        name = reader->getUtf8String();
        break;
    case 70:
        type = static_cast<dint16>(reader->getInt32());
        break;
    case 290:
        status = reader->getBool();
        break;
    case 63:
        lightColor = static_cast<dint16>(reader->getInt32());
        break;
    case 291:
        plotGlyph = reader->getBool();
        break;
    case 40:
        intensity = reader->getDouble();
        break;
    case 10:
        position.x = reader->getDouble();
        break;
    case 20:
        position.y = reader->getDouble();
        break;
    case 30:
        position.z = reader->getDouble();
        break;
    case 11:
        target.x = reader->getDouble();
        break;
    case 21:
        target.y = reader->getDouble();
        break;
    case 31:
        target.z = reader->getDouble();
        break;
    case 72:
        attenuationType = static_cast<dint16>(reader->getInt32());
        break;
    case 292:
        useAttenuationLimits = reader->getBool();
        break;
    case 41:
        attenuationStartLimit = reader->getDouble();
        break;
    case 42:
        attenuationEndLimit = reader->getDouble();
        break;
    case 50:
        hotspotAngle = reader->getDouble();
        break;
    case 51:
        falloffAngle = reader->getDouble();
        break;
    case 293:
        castShadows = reader->getBool();
        break;
    case 73:
        shadowType = static_cast<dint16>(reader->getInt32());
        break;
    case 91:
        shadowMapSize = static_cast<dint16>(reader->getInt32());
        break;
    case 280:
        shadowSoftness = static_cast<dint16>(reader->getInt32());
        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

//! DWG 路径的 LIGHT 位布局照 libredwg 的 dwg2.spec（DWG_ENTITY (LIGHT) 的
//! 非 DXF 分支）逐字段对齐：
//!     BL class_version → T name → BL type → B status → CMC light_color
//!     → B plot_glyph → BD intensity → 3BD position → 3BD target
//!     → BL attenuation_type → B use_attenuation_limits
//!     → BD attenuation_start_limit → BD attenuation_end_limit
//!     → BD hotspot_angle → BD falloff_angle → B cast_shadows
//!     → BL shadow_type → BS shadow_map_size → RC shadow_map_softness
//!
//! 未建模的「光度扩展块」（LIGHTINGUNITS = 2 的图纸才有）不读：实体的公共句柄段
//! 是框架按**对象末尾**定位的（parseDwgEntHandle），多出的类型数据不影响句柄。
bool DRW_Light::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    // 2007+ 的文本在独立字符串流里，必须用它的副本读文字（与 MTEXT/ATTRIB 同）
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff;
    }

    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing light *********************************************\n");

    classVersion = buf->getBitLong();
    name = sBuf->getVariableText(version, false);
    type = static_cast<dint16>(buf->getBitLong());
    status = buf->getBit();
    lightColor = static_cast<dint16>(buf->getCmColor(version));
    plotGlyph = buf->getBit();
    intensity = buf->getBitDouble();
    position = buf->get3BitDouble();
    target = buf->get3BitDouble();
    attenuationType = static_cast<dint16>(buf->getBitLong());
    useAttenuationLimits = buf->getBit();
    attenuationStartLimit = buf->getBitDouble();
    attenuationEndLimit = buf->getBitDouble();
    hotspotAngle = buf->getBitDouble();
    falloffAngle = buf->getBitDouble();
    castShadows = buf->getBit();
    shadowType = static_cast<dint16>(buf->getBitLong());
    shadowMapSize = static_cast<dint16>(buf->getBitShort());
    shadowSoftness = static_cast<dint16>(buf->getRawChar8());

    DRW_DBG("light name: "); DRW_DBG(name.c_str());
    DRW_DBG(", type: "); DRW_DBG(type);
    DRW_DBG(", size: "); DRW_DBGPT(position.x, position.y, position.z);
    DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    return buf->isGood();
}

void DRW_Line::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 11:
        secPoint.x = reader->getDouble();
        break;
    case 21:
        secPoint.y = reader->getDouble();
        break;
    case 31:
        secPoint.z = reader->getDouble();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

bool DRW_Line::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing line *********************************************\n");

    if (version < DRW::AC1015) {//14-
        basePoint.x = buf->getBitDouble();
        basePoint.y = buf->getBitDouble();
        basePoint.z = buf->getBitDouble();
        secPoint.x = buf->getBitDouble();
        secPoint.y = buf->getBitDouble();
        secPoint.z = buf->getBitDouble();
    }
    if (version > DRW::AC1014) {//2000+
        bool zIsZero = buf->getBit(); //B
        basePoint.x = buf->getRawDouble();//RD
        secPoint.x = buf->getDefaultDouble(basePoint.x);//DD
        basePoint.y = buf->getRawDouble();//RD
        secPoint.y = buf->getDefaultDouble(basePoint.y);//DD
        if (!zIsZero) {
            basePoint.z = buf->getRawDouble();//RD
            secPoint.z = buf->getDefaultDouble(basePoint.z);//DD
        }
    }
    DRW_DBG("start point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    DRW_DBG("\nend point: "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z);
    thickness = buf->getThickness(version > DRW::AC1014);//BD
    DRW_DBG("\nthickness: "); DRW_DBG(thickness);
    extPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG(", Extrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

bool DRW_Ray::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing ray/xline *********************************************\n");
    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    secPoint.x = buf->getBitDouble();
    secPoint.y = buf->getBitDouble();
    secPoint.z = buf->getBitDouble();
    DRW_DBG("start point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    DRW_DBG("\nvector: "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z);
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Circle::applyExtrusion(){
    if (haveExtrusion) {
        //NOTE: Commenting these out causes the the arcs being tested to be located
        //on the other side of the y axis (all x dimensions are negated).
        calculateAxis(extPoint);
        extrudePoint(extPoint, &basePoint);
    }
}

void DRW_Circle::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 40:
        radious = reader->getDouble();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

bool DRW_Circle::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing circle *********************************************\n");

    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("center: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    radious = buf->getBitDouble();
    DRW_DBG("\nradius: "); DRW_DBG(radious);

    thickness = buf->getThickness(version > DRW::AC1014);
    DRW_DBG(" thickness: "); DRW_DBG(thickness);
    extPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG("\nextrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z); DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Arc::applyExtrusion(){
    DRW_Circle::applyExtrusion();

    if(haveExtrusion){
        // If the extrusion vector has a z value less than 0, the angles for the arc
        // have to be mirrored since DXF files use the right hand rule.
        // Note that the following code only handles the special case where there is a 2D
        // drawing with the z axis heading into the paper (or rather screen). An arbitrary
        // extrusion axis (with x and y values greater than 1/64) may still have issues.
        if (fabs(extPoint.x) < 0.015625 && fabs(extPoint.y) < 0.015625 && extPoint.z < 0.0) {
            staangle=M_PI-staangle;
            endangle=M_PI-endangle;

            double temp = staangle;
            staangle=endangle;
            endangle=temp;
        }
    }
}

void DRW_Arc::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 50:
        staangle = reader->getDouble()/ ARAD;
        break;
    case 51:
        endangle = reader->getDouble()/ ARAD;
        break;
    default:
        DRW_Circle::parseCode(code, reader);
        break;
    }
}

bool DRW_Arc::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing circle arc *********************************************\n");

    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("center point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);

    radious = buf->getBitDouble();
    DRW_DBG("\nradius: "); DRW_DBG(radious);
    thickness = buf->getThickness(version > DRW::AC1014);
    DRW_DBG(" thickness: "); DRW_DBG(thickness);
    extPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG("\nextrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);
    staangle = buf->getBitDouble();
    DRW_DBG("\nstart angle: "); DRW_DBG(staangle);
    endangle = buf->getBitDouble();
    DRW_DBG(" end angle: "); DRW_DBG(endangle); DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    return buf->isGood();
}

void DRW_Ellipse::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 40:
        ratio = reader->getDouble();
        break;
    case 41:
        staparam = reader->getDouble();
        break;
    case 42:
        endparam = reader->getDouble();
        break;
    default:
        DRW_Line::parseCode(code, reader);
        break;
    }
}

void DRW_Ellipse::applyExtrusion(){
    if (haveExtrusion) {
        calculateAxis(extPoint);
        extrudePoint(extPoint, &secPoint);
        double intialparam = staparam;
        if (extPoint.z < 0.){
            staparam = M_PIx2 - endparam;
            endparam = M_PIx2 - intialparam;
        }
    }
}

//if ratio > 1 minor axis are greather than major axis, correct it
void DRW_Ellipse::correctAxis(){
    bool complete = false;
    if (staparam == endparam) {
        staparam = 0.0;
        endparam = M_PIx2; //2*M_PI;
        complete = true;
    }
    if (ratio > 1){
        if ( fabs(endparam - staparam - M_PIx2) < 1.0e-10)
            complete = true;
        double incX = secPoint.x;
        secPoint.x = -(secPoint.y * ratio);
        secPoint.y = incX*ratio;
        ratio = 1/ratio;
        if (!complete){
            if (staparam < M_PI_2)
                staparam += M_PI *2;
            if (endparam < M_PI_2)
                endparam += M_PI *2;
            endparam -= M_PI_2;
            staparam -= M_PI_2;
        }
    }
}

bool DRW_Ellipse::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing ellipse *********************************************\n");

    basePoint =buf->get3BitDouble();
    DRW_DBG("center: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    secPoint =buf->get3BitDouble();
    DRW_DBG(", axis: "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z); DRW_DBG("\n");
    extPoint =buf->get3BitDouble();
    DRW_DBG("Extrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);
    ratio = buf->getBitDouble();//BD
    DRW_DBG("\nratio: "); DRW_DBG(ratio);
    staparam = buf->getBitDouble();//BD
    DRW_DBG(" start param: "); DRW_DBG(staparam);
    endparam = buf->getBitDouble();//BD
    DRW_DBG(" end param: "); DRW_DBG(endparam); DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

//parts are the number of vertex to split polyline, default 128
void DRW_Ellipse::toPolyline(DRW_Polyline *pol, int parts){
    double radMajor, radMinor, cosRot, sinRot, incAngle, curAngle;
    double cosCurr, sinCurr;
    radMajor = sqrt(secPoint.x*secPoint.x + secPoint.y*secPoint.y);
    radMinor = radMajor*ratio;
    //calculate sin & cos of included angle
    incAngle = atan2(secPoint.y, secPoint.x);
    cosRot = cos(incAngle);
    sinRot = sin(incAngle);
    incAngle = M_PIx2 / parts;
    curAngle = staparam;
    int i = static_cast<int>(curAngle / incAngle);
    do {
        if (curAngle > endparam) {
            curAngle = endparam;
            i = parts+2;
        }
        cosCurr = cos(curAngle);
        sinCurr = sin(curAngle);
        double x = basePoint.x + (cosCurr*cosRot*radMajor) - (sinCurr*sinRot*radMinor);
        double y = basePoint.y + (cosCurr*sinRot*radMajor) + (sinCurr*cosRot*radMinor);
        pol->addVertex( DRW_Vertex(x, y, 0.0, 0.0));
        curAngle = (++i)*incAngle;
    } while (i<parts);
    if ( fabs(endparam - staparam - M_PIx2) < 1.0e-10){
        pol->flags = 1;
    }
    pol->layer = this->layer;
    pol->lineType = this->lineType;
    pol->color = this->color;
    pol->lWeight = this->lWeight;
    pol->extPoint = this->extPoint;
}

void DRW_Trace::applyExtrusion(){
    if (haveExtrusion) {
        calculateAxis(extPoint);
        extrudePoint(extPoint, &basePoint);
        extrudePoint(extPoint, &secPoint);
        extrudePoint(extPoint, &thirdPoint);
        extrudePoint(extPoint, &fourPoint);
    }
}

void DRW_Trace::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 12:
        thirdPoint.x = reader->getDouble();
        break;
    case 22:
        thirdPoint.y = reader->getDouble();
        break;
    case 32:
        thirdPoint.z = reader->getDouble();
        break;
    case 13:
        fourPoint.x = reader->getDouble();
        break;
    case 23:
        fourPoint.y = reader->getDouble();
        break;
    case 33:
        fourPoint.z = reader->getDouble();
        break;
    default:
        DRW_Line::parseCode(code, reader);
        break;
    }
}

bool DRW_Trace::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing Trace *********************************************\n");

    thickness = buf->getThickness(version>DRW::AC1014);
    basePoint.z = buf->getBitDouble();
    basePoint.x = buf->getRawDouble();
    basePoint.y = buf->getRawDouble();
    secPoint.x = buf->getRawDouble();
    secPoint.y = buf->getRawDouble();
    secPoint.z = basePoint.z;
    thirdPoint.x = buf->getRawDouble();
    thirdPoint.y = buf->getRawDouble();
    thirdPoint.z = basePoint.z;
    fourPoint.x = buf->getRawDouble();
    fourPoint.y = buf->getRawDouble();
    fourPoint.z = basePoint.z;
    extPoint = buf->getExtrusion(version>DRW::AC1014);

    DRW_DBG(" - base "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    DRW_DBG("\n - sec "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z);
    DRW_DBG("\n - third "); DRW_DBGPT(thirdPoint.x, thirdPoint.y, thirdPoint.z);
    DRW_DBG("\n - fourth "); DRW_DBGPT(fourPoint.x, fourPoint.y, fourPoint.z);
    DRW_DBG("\n - extrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);
    DRW_DBG("\n - thickness: "); DRW_DBG(thickness); DRW_DBG("\n");

    /* Common Entity Handle Data */
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;

    /* CRC X --- */
    return buf->isGood();
}


void DRW_Solid::parseCode(int code, dxfReader *reader){
    DRW_Trace::parseCode(code, reader);
}

bool DRW_Solid::parseDwg(DRW::Version v, dwgBuffer *buf, duint32 bs){
    DRW_DBG("\n***************************** parsing Solid *********************************************\n");
    return DRW_Trace::parseDwg(v, buf, bs);
}

void DRW_3Dface::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 70:
        invisibleflag = reader->getInt32();
        break;
    default:
        DRW_Trace::parseCode(code, reader);
        break;
    }
}

bool DRW_3Dface::parseDwg(DRW::Version v, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(v, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing 3Dface *********************************************\n");

    if ( v < DRW::AC1015 ) {// R13 & R14
        basePoint.x = buf->getBitDouble();
        basePoint.y = buf->getBitDouble();
        basePoint.z = buf->getBitDouble();
        secPoint.x = buf->getBitDouble();
        secPoint.y = buf->getBitDouble();
        secPoint.z = buf->getBitDouble();
        thirdPoint.x = buf->getBitDouble();
        thirdPoint.y = buf->getBitDouble();
        thirdPoint.z = buf->getBitDouble();
        fourPoint.x = buf->getBitDouble();
        fourPoint.y = buf->getBitDouble();
        fourPoint.z = buf->getBitDouble();
        invisibleflag = buf->getBitShort();
    } else { // 2000+
        bool has_no_flag = buf->getBit();
        bool z_is_zero = buf->getBit();
        basePoint.x = buf->getRawDouble();
        basePoint.y = buf->getRawDouble();
        basePoint.z = z_is_zero ? 0.0 : buf->getRawDouble();
        secPoint.x = buf->getDefaultDouble(basePoint.x);
        secPoint.y = buf->getDefaultDouble(basePoint.y);
        secPoint.z = buf->getDefaultDouble(basePoint.z);
        thirdPoint.x = buf->getDefaultDouble(secPoint.x);
        thirdPoint.y = buf->getDefaultDouble(secPoint.y);
        thirdPoint.z = buf->getDefaultDouble(secPoint.z);
        fourPoint.x = buf->getDefaultDouble(thirdPoint.x);
        fourPoint.y = buf->getDefaultDouble(thirdPoint.y);
        fourPoint.z = buf->getDefaultDouble(thirdPoint.z);
        invisibleflag = has_no_flag ? (int)NoEdge : buf->getBitShort();
    }
    drw_assert(invisibleflag>=NoEdge);
    drw_assert(invisibleflag<=AllEdges);

    DRW_DBG(" - base "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z); DRW_DBG("\n");
    DRW_DBG(" - sec "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z); DRW_DBG("\n");
    DRW_DBG(" - third "); DRW_DBGPT(thirdPoint.x, thirdPoint.y, thirdPoint.z); DRW_DBG("\n");
    DRW_DBG(" - fourth "); DRW_DBGPT(fourPoint.x, fourPoint.y, fourPoint.z); DRW_DBG("\n");
    DRW_DBG(" - Invisibility mask: "); DRW_DBG(invisibleflag); DRW_DBG("\n");

    /* Common Entity Handle Data */
    ret = DRW_Entity::parseDwgEntHandle(v, buf);
    if (!ret)
        return ret;
    return buf->isGood();
}

void DRW_Block::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        name = reader->getUtf8String();
        break;
    case 70:
        flags = reader->getInt32();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

bool DRW_Block::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    if (!isEnd){
        DRW_DBG("\n***************************** parsing block *********************************************\n");
        name = sBuf->getVariableText(version, false);
        DRW_DBG("Block name: "); DRW_DBG(name.c_str()); DRW_DBG("\n");
    } else {
        DRW_DBG("\n***************************** parsing end block *********************************************\n");
    }
    if (version > DRW::AC1018) {//2007+
        duint8 unk = buf->getBit();
        DRW_DBG("unknown bit: "); DRW_DBG(unk); DRW_DBG("\n");
    }
//    X handleAssoc;   //X
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Insert::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        name = reader->getUtf8String();
        break;
    case 41:
        xscale = reader->getDouble();
        break;
    case 42:
        yscale = reader->getDouble();
        break;
    case 43:
        zscale = reader->getDouble();
        break;
    case 50:
        angle = reader->getDouble();
        angle = angle/ARAD; //convert to radian
        break;
    case 70:
        colcount = reader->getInt32();
        break;
    case 71:
        rowcount = reader->getInt32();
        break;
    case 44:
        colspace = reader->getDouble();
        break;
    case 45:
        rowspace = reader->getDouble();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

// DRW_Table 的完整解析在 dxfRW::processTable 里完成（两段子类标记需要
// 上下文状态，单靠 parseCode 无法区分同名组码）。这里只处理兜底路径——
// 当调用方直接把组码喂进来时，能解析的就解析。
void DRW_Table::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        tableStyle = reader->getUtf8String();
        break;
    case 91:
        numRows = reader->getInt32();
        break;
    case 92:
        numCols = reader->getInt32();
        break;
    case 141:
        rowHeights.push_back(reader->getDouble());
        break;
    case 142:
        colWidths.push_back(reader->getDouble());
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

namespace {

// 行列数上限：垃圾计数（错位解析出的天文数字）不至于撑爆内存。
constexpr dint32 kTableMaxDim = 5000;

dint32 tableSafeCount(dint32 raw, dint32 cap = kTableMaxDim)
{
    if (raw < 0) return 0;
    return raw > cap ? cap : raw;
}

/// UTF-16LE 字节串 → UTF-8。AcDbCellValue 的字符串值带 2 字节结尾 NUL。
std::string tableUtf16ToUtf8(const duint8* data, dint32 bytes)
{
    std::string out;
    if (!data || bytes <= 0) return out;
    const dint32 units = bytes / 2;
    out.reserve(static_cast<std::size_t>(units));
    for (dint32 i = 0; i < units; ++i) {
        const duint32 u = static_cast<duint32>(data[2 * i])
                        | (static_cast<duint32>(data[2 * i + 1]) << 8);
        if (u == 0) break;                       // 结尾 NUL
        if (u < 0x80) {
            out += static_cast<char>(u);
        } else if (u < 0x800) {
            out += static_cast<char>(0xC0 | (u >> 6));
            out += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (u >> 12));
            out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return out;
}

} // namespace

bool DRW_Table::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    // ACAD_TABLE 的 DWG 二进制布局（依据 opencadcodec 的 read_table /
    // read_table_content / read_table_cell 系列实测位布局移植；旧实现的
    // 「cells 编码未知」段落已按参考实现补全）。
    //
    // ★ 身份判定：ACAD_TABLE 是**实体**（entity），不是对象（object）。
    //   判据来自 example.dwg 的 CLASSES 段实测：item_class_id 498 (0x1F2)
    //   即判定为实体。公共头是实体公共数据（owner/link/layer 句柄链、preview
    //   标志、entmode、numReactors…），由 DRW_Entity::parseDwg 负责。
    //
    // ★ 对象数据分三股流，需要三个独立游标：
    //   · 数据流  buf ：实体公共数据之后继续；
    //   · 句柄流  hbuf：自 objSize 位偏移起（R2000/2004/2007 是 bitsize RL，
    //     2010+ 是「总位数 - UMC 句柄流位数」，两者都落在同一约定上）；
    //   · 字符串流 sbuf：2007+（AC1021 及以上）才有，起点由 objSize-1 位上的
    //     标志位给出——定位逻辑与 DRW_Entity::parseDwg 的 strBuf 完全一致；
    //     2007 以下文字内联在数据流，sbuf 即 buf。
    //
    //   两条解析路径：
    //   · R2000 ~ R2007（< AC1024）：扁平格式 —— 90 标志 / 11 水平方向 /
    //     92 列数 / 91 行数 / 142 列宽×n / 141 行高×n / 表样式句柄（句柄流）/
    //     rows×cols 单元格 / 4 个表级替代 bit；
    //     单元格 = BS 类型 + RC 边框标志 + B merged + B autofit + BL 跨列 +
    //     BL 跨行 + BD 旋转 + 值句柄（句柄流）+ [文字 TV（2007 以下内联）]
    //     + [单元格样式替代] + [2007+ AcDbCellValue]。
    //   · R2010+（≥ AC1024）：完整 TABLECONTENT —— 未知 RC/H/BL/(BL|B) 后接
    //     列表（列名/自定义/样式/列宽）、行表（每行 ncells 个单元格）、
    //     字段句柄表、基样式、合并区（top/left/bottom/right）、表样式句柄、
    //     未知 BS、水平方向、断裂信息。
    //
    //   任一环节解析失败即清空结构化数据，上层退回 *Tnn 几何块反推兜底，
    //   不会比旧行为更差。
    DRW_DBG("\n************************** parsing ACAD_TABLE *****************************************\n");

    // ---- 实体公共数据（含 bitsize/handle/EED/preview/entmode/numReactors/颜色…）----
    if (!DRW_Entity::parseDwg(version, buf, nullptr, bs))
        return false;
    DRW_DBG(" objSize: "); DRW_DBG(objSize);
    DRW_DBG(", numReactors: "); DRW_DBG(numReactors); DRW_DBG("\n");

    // ---- AcDbBlockReference 段落（数据流） --------------------------------
    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("insertion point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z); DRW_DBG("\n");

    // 比例编码与 INSERT 相同（bit pair）：
    //   0 = 三个都读；1 = x 默认 1、y/z 可随 1；2 = x==y==z；3 = 全 1,1,1。
    duint8 scaleFlag = buf->get2Bits();
    if (scaleFlag == 0) {
        xscale = buf->getRawDouble();
        yscale = buf->getDefaultDouble(xscale);
        zscale = buf->getDefaultDouble(xscale);
    } else if (scaleFlag == 1) {
        xscale = 1.0;
        yscale = buf->getDefaultDouble(1.0);
        zscale = buf->getDefaultDouble(1.0);
    } else if (scaleFlag == 2) {
        xscale = buf->getRawDouble();
        yscale = zscale = xscale;
    } else { // 3
        xscale = yscale = zscale = 1.0;
    }
    DRW_DBG("scale : "); DRW_DBGPT(xscale, yscale, zscale); DRW_DBG("\n");

    angle = buf->getBitDouble();
    DRW_DBG(", angle: "); DRW_DBG(angle); DRW_DBG("\n");

    extPoint = buf->getExtrusion(false); //3BD R14 style

    bool hasAttrib = buf->getBit();
    DRW_DBG("   has Attrib: "); DRW_DBG(hasAttrib);

    // 2004+ 的「所拥有对象数」只在 has_attribs 为真时存在——它与 INSERT 是
    // 同一个字段（DRW_Insert::parseDwg 同为 `if (hasAttrib && version > AC1015)`），
    // 语义是「本实体拥有的 ATTRIB/SEQEND 个数」。
    // 表格的 has_attribs 恒为 false，AutoCAD 不写该字段；此前的无条件读取会让
    // 数据流错位 3 位，把 num_cols / num_rows 读成垃圾值。
    dint32 numOwned = 0;
    if (hasAttrib && version > DRW::AC1015) {
        numOwned = buf->getBitLong();
        DRW_DBG(", numOwned: "); DRW_DBG(numOwned);
    }
    DRW_DBG("\n");

    // ---- 三个游标就位 -------------------------------------------------------
    // 句柄流：克隆一份缓冲，绝对定位到 objSize 位偏移。
    dwgBuffer hbuf = *buf;
    hbuf.setPosition(objSize >> 3);
    hbuf.setBitPos(objSize & 7);

    // 字符串流：2007+ 才独立存在；克隆后按标志位舞步定位（与 parseDwg 一致）。
    dwgBuffer sbufStore = *buf;
    sbufStore.resetPosition();
    dwgBuffer* sbuf = buf;
    if (version > DRW::AC1018) {
        sbufStore.moveBitPos(objSize - 1);
        if (sbufStore.getBit() == 1) {
            sbufStore.moveBitPos(-17);
            duint16 strDataSize = sbufStore.getRawShort16();
            if ((strDataSize & 0x8000) == 0x8000) {
                sbufStore.moveBitPos(-33);
                duint16 hiSize = sbufStore.getRawShort16();
                strDataSize = static_cast<duint16>((strDataSize & 0x7FFF) | (hiSize << 15));
            }
            sbufStore.moveBitPos(-static_cast<dint32>(strDataSize) - 16);
        }
        sbuf = &sbufStore;
    }

    cells.clear();
    merges.clear();

    // 变长文本：2007+ 走字符串流，2000/2004 内联（TV = BS 长度 + 代码页字节）。
    auto tvString = [&](bool& ok) -> UTF8STRING {
        if (!ok) return UTF8STRING();
        const UTF8STRING s = sbuf->getVariableText(version, false);
        if (!sbuf->isGood()) ok = false;
        return s;
    };

    // AcDbCellValue（2007+ 的单元格值）。只取文字，其余类型按位布局读掉。
    auto readCadValue = [&](bool& ok) -> UTF8STRING {
        if (!ok) return UTF8STRING();
        const dint32 flags = buf->getBitLong();          // 值标志（bit0 抑制值体）
        const dint32 tc = buf->getBitLong();             // 类型码
        UTF8STRING text;
        if ((flags & 1) == 0) {
            switch (tc) {
            case 0: case 1:
                buf->getBitLong();                        // 整数
                break;
            case 2:
                buf->getBitDouble();                      // 实数
                break;
            case 4: case 0x200: {
                // BL 字节数 + UTF-16LE（含 2 字节结尾 NUL）
                const dint32 len = tableSafeCount(buf->getBitLong(), 1 << 20);
                if (len > 0) {
                    std::vector<duint8> bytes(static_cast<std::size_t>(len));
                    ok = buf->getBytes(bytes.data(), len) && buf->isGood();
                    if (ok) text = tableUtf16ToUtf8(bytes.data(), len);
                }
                break;
            }
            case 8: {                                     // 二进制
                const dint32 n = tableSafeCount(buf->getBitLong(), 1 << 24);
                for (dint32 i = 0; ok && i < n; ++i) buf->getRawChar8();
                break;
            }
            case 0x10: {                                  // 2D 点
                const dint32 n = tableSafeCount(buf->getBitLong(), 1 << 24);
                for (dint32 i = 0; ok && i < n; ++i) { buf->getRawDouble(); buf->getRawDouble(); }
                break;
            }
            case 0x20: {                                  // 3D 点
                const dint32 n = tableSafeCount(buf->getBitLong(), 1 << 24);
                for (dint32 i = 0; ok && i < n; ++i) { buf->getRawDouble(); buf->getRawDouble(); buf->getRawDouble(); }
                break;
            }
            case 0x40:
                hbuf.getHandle();                         // 句柄值
                break;
            case 0x80: case 0x100:                        // 无体
            default:
                break;
            }
        }
        if (ok) {
            const dint32 unit = buf->getBitLong();        // 单位类型
            tvString(ok);                                 // 格式串
            if (unit != 12) tvString(ok);                 // 已格式化值
        }
        if (!buf->isGood() || !hbuf.isGood()) ok = false;
        return text;
    };

    // 内容格式（对齐/旋转/文字样式…）。返回对齐方式（1~9，越界归 5）。
    auto readContentFormat = [&](bool& ok) -> int {
        if (!ok) return 5;
        buf->getBitLong();                                // 替代标志
        buf->getBitLong();                                // 属性标志
        buf->getBitLong();                                // 值数据类型
        buf->getBitLong();                                // 值单位类型
        tvString(ok);                                     // 值格式串
        buf->getBitDouble();                              // 旋转
        buf->getBitDouble();                              // 缩放
        int align = static_cast<int>(buf->getBitLong());  // 对齐
        buf->getCmColor(version);                         // 内容颜色
        hbuf.getHandle();                                 // 文字样式
        buf->getBitDouble();                              // 文字高度
        if (align < 1 || align > 9) align = 5;
        if (!buf->isGood() || !hbuf.isGood()) ok = false;
        return align;
    };

    // 单元格/行/列/基样式共用的样式块。dataFlags == 0 即无数据。
    auto readCellStyle = [&](bool& ok) {
        if (!ok) return;
        buf->getBitLong();                                // 样式类型
        const dint16 dataFlags = buf->getBitShort();
        if (dataFlags == 0) return;
        buf->getBitLong();                                // 替代标志
        buf->getBitLong();                                // 属性标志
        buf->getCmColor(version);                         // 背景色
        buf->getBitLong();                                // 布局标志
        readContentFormat(ok);
        const dint16 marginFlags = buf->getBitShort();
        if (marginFlags & 0x01) {
            for (int i = 0; ok && i < 6; ++i) buf->getBitDouble();   // 四边距+横竖间距
        }
        const dint32 nBorders = tableSafeCount(buf->getBitLong());
        for (dint32 i = 0; ok && i < nBorders; ++i) {
            const duint32 edge = static_cast<duint32>(buf->getBitLong());
            if (edge == 1 || edge == 2 || edge == 4 || edge == 8
                || edge == 0x10 || edge == 0x20) {
                buf->getBitLong();                        // 替代标志
                buf->getBitLong();                        // 边框类型
                buf->getCmColor(version);                 // 颜色
                buf->getBitLong();                        // 线宽
                hbuf.getHandle();                         // 线型
                buf->getBitLong();                        // 不可见
                buf->getBitDouble();                      // 双线间距
            }
            if (!buf->isGood() || !hbuf.isGood()) ok = false;
        }
        if (!buf->isGood() || !hbuf.isGood()) ok = false;
    };

    // ---- 句柄流公共段：owner/xdic/reactors/prev/next/layer → block_header
    //      → [attribs…seqend] → tablestyle（仅 2010 以前在此处）------------
    auto advanceCommonHandles = [&]() -> bool {
        if (!DRW_Entity::parseDwgEntHandle(version, &hbuf))
            return false;
        blockRecH = hbuf.getHandle();
        DRW_DBG("BLOCK HEADER Handle: "); DRW_DBGHL(blockRecH.code, blockRecH.size, blockRecH.ref); DRW_DBG("\n");
        if (hasAttrib) {
            if (version < DRW::AC1018) {          // 2000-
                hbuf.getHandle();                 // first attrib
                hbuf.getHandle();                 // last attrib
            } else {
                for (dint32 i = 0; i < numOwned && i < 10000; ++i)
                    hbuf.getHandle();
            }
            hbuf.getHandle();                     // seqend
        }
        if (version < DRW::AC1024) {
            tableStyleH = hbuf.getHandle();
            DRW_DBG("tableStyle handle: "); DRW_DBGHL(tableStyleH.code, tableStyleH.size, tableStyleH.ref); DRW_DBG("\n");
        }
        return hbuf.isGood();
    };

    if (version < DRW::AC1024) {
        // ---- R2000 ~ R2007：扁平格式（数据流） ----------------------------
        if (version >= DRW::AC1015) {
            buf->getBitShort();                   /* 90 flag_for_table_value */
            buf->get3BitDouble();                 /* 11/21/31 水平方向（丢弃） */

            numCols = tableSafeCount(buf->getBitLong());
            numRows = tableSafeCount(buf->getBitLong());
            DRW_DBG("rows: "); DRW_DBG(numRows); DRW_DBG(", cols: "); DRW_DBG(numCols); DRW_DBG("\n");

            if (numCols > 0) {
                colWidths.reserve(static_cast<size_t>(numCols));
                for (int i = 0; i < numCols; ++i)
                    colWidths.push_back(buf->getBitDouble());
            } else {
                numCols = 0;
            }
            if (numRows > 0) {
                rowHeights.reserve(static_cast<size_t>(numRows));
                for (int i = 0; i < numRows; ++i)
                    rowHeights.push_back(buf->getBitDouble());
            } else {
                numRows = 0;
            }

            // 句柄流公共段先走完，之后才是各单元格的值句柄
            advanceCommonHandles();
            bool ok = buf->isGood() && hbuf.isGood();

            for (int r = 0; ok && r < numRows; ++r) {
                for (int c = 0; ok && c < numCols; ++c) {
                    const dint16 ctype = buf->getBitShort();   // 1 文字 / 2 块 / 0 空
                    buf->getRawChar8();                        // 边框标志
                    buf->getBit();                             // merged
                    buf->getBit();                             // auto fit
                    const dint32 mergeW = buf->getBitLong();   // 跨列数
                    const dint32 mergeH = buf->getBitLong();   // 跨行数
                    buf->getBitDouble();                       // 旋转
                    const dwgHandle valueH = hbuf.getHandle(); // 单元格值句柄
                    int align = 5;

                    if (ctype == 1) {
                        if (version < DRW::AC1021 && valueH.ref == 0) {
                            // 2000/2004：文字内联（TV）；值句柄非 0 时文字在别处
                            Cell cell;
                            cell.row = r;
                            cell.col = c;
                            cell.text = buf->getVariableText(version, false);
                            if (!buf->isGood()) ok = false;
                            if (ok && !cell.text.empty()) cells.push_back(cell);
                        }
                    } else if (ctype == 2) {
                        buf->getBitDouble();                   // 块缩放
                        if (buf->getBit()) {
                            const dint16 natts = buf->getBitShort();
                            for (dint16 a = 0; ok && a < natts; ++a) {
                                hbuf.getHandle();              // 属性定义
                                buf->getBitShort();            // 索引
                                buf->getVariableText(version, false);   // 值
                            }
                        }
                    }

                    if (buf->getBit()) {                       // 单元格样式替代
                        const dint32 flags = buf->getBitLong();
                        buf->getRawChar8();                    // virtual edge
                        if (flags & 0x01) align = buf->getBitShort();
                        if (flags & 0x02) buf->getBit();       // fill
                        if (flags & 0x04) buf->getCmColor(version);   // 背景色
                        if (flags & 0x08) buf->getCmColor(version);   // 内容色
                        if (flags & 0x10) hbuf.getHandle();    // 文字样式
                        if (flags & 0x20) buf->getBitDouble(); // 文字高度
                        for (int edge = 0; ok && edge < 4; ++edge) {
                            const dint32 colorBit = 0x40 << edge;
                            const dint32 lwBit = 0x400 << edge;
                            if (flags & colorBit) buf->getCmColor(version);
                            if (flags & lwBit) { buf->getBitShort(); buf->getBitShort(); }
                        }
                        if (align < 1 || align > 9) align = 5;
                    }

                    if (ok && version >= DRW::AC1021) {
                        // 2007+：格尾先有一个 cell.flag（BL，通常为 0），其后才是
                        // 承载文字的 AcDbCellValue。漏读这个 BL 会让**每格少读 2 位**
                        // （BL 短码），9 格累计错位 18 位，其后的「4 个表级替代 bit」
                        // 与对象末尾全部对不上 —— 实测对象解析返回 false，进而整份
                        // 图纸读取失败（BAD_READ_ENTITIES）。参考实现的读取端即
                        // read_table_cell_data 里 `cell.flag = reader.read_bit_long()`。
                        buf->getBitLong();                    // cell.flag
                        Cell cell;
                        cell.row = r;
                        cell.col = c;
                        cell.text = readCadValue(ok);
                        if (ok && !cell.text.empty()) cells.push_back(cell);
                    }

                    if (ok && (mergeW > 1 || mergeH > 1)) {
                        Merge merge;
                        merge.row = r;
                        merge.col = c;
                        merge.rowSpan = static_cast<int>(std::min<dint32>(mergeH, numRows - r));
                        merge.colSpan = static_cast<int>(std::min<dint32>(mergeW, numCols - c));
                        if (merge.rowSpan > 1 || merge.colSpan > 1) merges.push_back(merge);
                    }

                    if (!buf->isGood() || !hbuf.isGood()) ok = false;
                }
            }

            if (ok) {
                // 表级替代：4 个 bit 门控（通常全 0，读掉以保持对齐）
                auto readBorderList = [&](bool colors) {
                    const dint32 flags = buf->getBitLong();
                    for (int bit = 0; bit < 18; ++bit) {
                        if (flags & (1 << bit)) {
                            if (colors) buf->getCmColor(version);
                            else buf->getBitShort();
                        }
                    }
                };
                if (buf->getBit()) {
                    // 表样式替代
                    const dint32 flags = buf->getBitLong();
                    if (flags & 0x0001) buf->getBit();
                    if (flags & 0x0002) buf->getBit();
                    if (flags & 0x0004) buf->getBitShort();
                    if (flags & 0x0008) buf->getBitDouble();
                    if (flags & 0x0010) buf->getBitDouble();
                    for (int bit : {0x0020, 0x0040, 0x0080})
                        if (flags & bit) buf->getCmColor(version);
                    for (int bit : {0x0100, 0x0200, 0x0400})
                        if (flags & bit) buf->getBit();
                    for (int bit : {0x0800, 0x1000, 0x2000})
                        if (flags & bit) buf->getCmColor(version);
                    for (int bit : {0x4000, 0x8000, 0x10000})
                        if (flags & bit) buf->getBitShort();
                    for (int bit : {0x20000, 0x40000, 0x80000})
                        if (flags & bit) hbuf.getHandle();
                    for (int bit : {0x100000, 0x200000, 0x400000})
                        if (flags & bit) buf->getBitDouble();
                }
                if (buf->getBit()) readBorderList(true);      // 边框颜色替代
                if (buf->getBit()) readBorderList(false);     // 边框线宽替代
                if (buf->getBit()) readBorderList(false);     // 边框可见性替代
                ok = buf->isGood() && hbuf.isGood();
            }

            if (!ok) {
                // 解析失败：清空结构化数据，上层退回 *Tnn 几何块反推兜底
                DRW_DBG("ACAD_TABLE flat cells parse failed, fallback to geometry\n");
                cells.clear();
                merges.clear();
            }
        }
    } else {
        // ---- R2010+：完整 TABLECONTENT（AcDbLinkedTableData） -------------
        // 单元格内容里的文字/合并区按参考实现逐字段读出。
        //
        // ★ 句柄流的公共段必须先走完。R2010 分支此前**漏调** advanceCommonHandles()，
        //   于是实体公共句柄（属主 / 图层）与 block_header 从未被消费：
        //     · layer 取不到 → 读回后图层恒为 0（实测「原 明细表，读 0」）；
        //     · 句柄游标整体前移两个句柄 → 其后的未知句柄、单元格样式句柄、
        //       表样式句柄全部读到错位的位置。
        //   扁平分支（R2000~R2007）一直是调用的，R2010 漏了。
        advanceCommonHandles();

        buf->getRawChar8();                       // 未知字节
        hbuf.getHandle();                         // 未知句柄
        buf->getBitLong();                        // 未知长整数
        if (version >= DRW::AC1027) buf->getBitLong();
        else buf->getBit();                       // R2010 是单个 bit

        bool ok = buf->isGood() && hbuf.isGood();

        // 单元格（行内第 k 格）。首个 Value 内容的文字即单元格文字。
        auto readTableCell = [&](int r, int k) {
            buf->getBitLong();                    // 状态
            tvString(ok);                         // 提示
            buf->getBitLong();                    // custom
            const dint32 nItems = tableSafeCount(buf->getBitLong());
            for (dint32 i = 0; ok && i < nItems; ++i) {
                tvString(ok);                     // 自定义名
                readCadValue(ok);                 // 自定义值
            }
            if (buf->getBitLong() == 1) {         // 链接数据
                hbuf.getHandle();
                buf->getBitLong(); buf->getBitLong(); buf->getBitLong();
            }
            const dint32 nContents = tableSafeCount(buf->getBitLong(), 1000);
            int align = 5;
            UTF8STRING text;
            for (dint32 i = 0; ok && i < nContents; ++i) {
                const dint32 ct = buf->getBitLong();
                if (ct == 1) {                    // 值
                    const UTF8STRING t = readCadValue(ok);
                    if (text.empty()) text = t;
                } else if (ct == 2) {             // FIELD
                    hbuf.getHandle();
                } else if (ct == 4) {             // 块
                    hbuf.getHandle();
                } else {
                    // 未知内容类型：无体
                }
                const dint32 nAtts = tableSafeCount(buf->getBitLong());
                for (dint32 a = 0; ok && a < nAtts; ++a) {
                    hbuf.getHandle();             // 属性定义
                    tvString(ok);                 // 属性值
                    buf->getBitLong();            // 索引
                }
                if (buf->getBitShort() != 0) {
                    const int a = readContentFormat(ok);
                    if (a >= 1 && a <= 9) align = a;
                }
            }
            readCellStyle(ok);
            buf->getBitLong();                    // style id
            if (buf->getBitLong() != 0) {         // 单元格几何
                buf->getBitLong();                // 几何标志
                buf->getBitDouble();              // 含间隙宽
                buf->getBitDouble();              // 含间隙高
                hbuf.getHandle();
                const dint32 nGeom = tableSafeCount(buf->getBitLong());
                for (dint32 g = 0; ok && g < nGeom; ++g) {
                    buf->get3BitDouble();         // 到左上距离
                    buf->get3BitDouble();         // 到中心距离
                    buf->getBitDouble();          // 宽
                    buf->getBitDouble();          // 高
                    buf->getBitDouble();          // 外宽
                    buf->getBitDouble();          // 外高
                    buf->getBitLong();            // 标志
                }
            }
            if (ok && !text.empty()) {
                Cell cell;
                cell.row = r;
                cell.col = k;
                cell.text = text;
                cell.align = align;
                cells.push_back(cell);
            }
            if (!buf->isGood() || !hbuf.isGood()) ok = false;
        };

        if (ok) {
            tvString(ok);                         // 表名
            tvString(ok);                         // 描述

            numCols = tableSafeCount(buf->getBitLong());
            for (int c = 0; ok && c < numCols; ++c) {
                tvString(ok);                     // 列名
                buf->getBitLong();                // custom
                const dint32 nItems = tableSafeCount(buf->getBitLong());
                for (dint32 i = 0; ok && i < nItems; ++i) {
                    tvString(ok);
                    readCadValue(ok);
                }
                readCellStyle(ok);                // 列样式
                buf->getBitLong();                // style id
                if (ok) colWidths.push_back(buf->getBitDouble());
            }
            if (!buf->isGood() || !hbuf.isGood()) ok = false;
            if (!ok) numCols = 0;

            numRows = tableSafeCount(buf->getBitLong());
            for (int r = 0; ok && r < numRows; ++r) {
                const dint32 ncells = tableSafeCount(buf->getBitLong());
                for (dint32 k = 0; ok && k < ncells; ++k)
                    readTableCell(r, static_cast<int>(k));
                buf->getBitLong();                // custom
                const dint32 nItems = tableSafeCount(buf->getBitLong());
                for (dint32 i = 0; ok && i < nItems; ++i) {
                    tvString(ok);
                    readCadValue(ok);
                }
                readCellStyle(ok);                // 行样式
                buf->getBitLong();                // style id
                if (ok) rowHeights.push_back(buf->getBitDouble());
            }
            if (!buf->isGood() || !hbuf.isGood()) ok = false;
            if (!ok) numRows = 0;

            const dint32 nFields = tableSafeCount(buf->getBitLong());
            for (dint32 i = 0; ok && i < nFields; ++i)
                hbuf.getHandle();
            readCellStyle(ok);                    // 基样式

            const dint32 nRanges = tableSafeCount(buf->getBitLong());
            for (dint32 i = 0; ok && i < nRanges; ++i) {
                const dint32 top = buf->getBitLong();
                const dint32 left = buf->getBitLong();
                const dint32 bottom = buf->getBitLong();
                const dint32 right = buf->getBitLong();
                if (top >= 0 && left >= 0 && bottom >= top && right >= left) {
                    Merge merge;
                    merge.row = top;
                    merge.col = left;
                    merge.rowSpan = bottom - top + 1;
                    merge.colSpan = right - left + 1;
                    merges.push_back(merge);
                }
            }

            tableStyleH = hbuf.getHandle();       // H 342 表样式
            DRW_DBG("tableStyle handle: "); DRW_DBGHL(tableStyleH.code, tableStyleH.size, tableStyleH.ref); DRW_DBG("\n");

            buf->getBitShort();                   // 未知 BS
            buf->get3BitDouble();                 // 11/21/31 水平方向（丢弃）
            if (buf->getBitLong() == 1) {         // 断裂数据
                buf->getBitLong();                // 选项
                buf->getBitLong();                // 流向
                buf->getBitDouble();              // 间距
                buf->getBitLong();
                buf->getBitLong();
                const dint32 n = tableSafeCount(buf->getBitLong());
                for (dint32 i = 0; ok && i < n; ++i) {
                    buf->get3BitDouble();         // 位置
                    buf->getBitDouble();          // 高
                    buf->getBitLong();            // 标志
                }
            }
            const dint32 nBreakRanges = tableSafeCount(buf->getBitLong());
            for (dint32 i = 0; ok && i < nBreakRanges; ++i) {
                buf->get3BitDouble();             // 位置
                buf->getBitLong();                // 起始行
                buf->getBitLong();                // 结束行
            }

            ok = buf->isGood() && hbuf.isGood();
        }

        if (!ok) {
            // 解析失败：清空结构化数据，上层退回 *Tnn 几何块反推兜底
            DRW_DBG("ACAD_TABLE content parse failed, fallback to geometry\n");
            cells.clear();
            merges.clear();
            rowHeights.clear();
            colWidths.clear();
            numRows = 0;
            numCols = 0;
        }
    }

    DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    return buf->isGood();
}

bool DRW_Insert::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dint32 objCount = 0;
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n************************** parsing insert/minsert *****************************************\n");
    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("insertion point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z); DRW_DBG("\n");
    if (version < DRW::AC1015) {//14-
        xscale = buf->getBitDouble();
        yscale = buf->getBitDouble();
        zscale = buf->getBitDouble();
    } else {
        duint8 dataFlags = buf->get2Bits();
        if (dataFlags == 3){
            //none default value 1,1,1
        } else if (dataFlags == 1){ //x default value 1, y & z can be x value
            yscale = buf->getDefaultDouble(xscale);
            zscale = buf->getDefaultDouble(xscale);
        } else if (dataFlags == 2){
            xscale = buf->getRawDouble();
            yscale = zscale = xscale;
        } else { //dataFlags == 0
            xscale = buf->getRawDouble();
            yscale = buf->getDefaultDouble(xscale);
            zscale = buf->getDefaultDouble(xscale);
        }
    }
    angle = buf->getBitDouble();
    DRW_DBG("scale : "); DRW_DBGPT(xscale, yscale, zscale); DRW_DBG(", angle: "); DRW_DBG(angle);
    extPoint = buf->getExtrusion(false); //3BD R14 style
    DRW_DBG("\nextrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);

    bool hasAttrib = buf->getBit();
    DRW_DBG("   has Attrib: "); DRW_DBG(hasAttrib);

    if (hasAttrib && version > DRW::AC1015) {//2004+
        objCount = buf->getBitLong();
        DRW_UNUSED(objCount);
        DRW_DBG("   objCount: "); DRW_DBG(objCount); DRW_DBG("\n");
    }
    if (oType == 8) {//entity are minsert
        colcount = buf->getBitShort();
        rowcount = buf->getBitShort();
        colspace = buf->getBitDouble();
        rowspace = buf->getBitDouble();
    }
    DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    blockRecH = buf->getHandle(); /* H 2 BLOCK HEADER (hard pointer) */
    DRW_DBG("BLOCK HEADER Handle: "); DRW_DBGHL(blockRecH.code, blockRecH.size, blockRecH.ref); DRW_DBG("\n");
    DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    /*attribs follows*/
    if (hasAttrib) {
        if (version < DRW::AC1018) {//2000-
            dwgHandle attH = buf->getHandle(); /* H 2 BLOCK HEADER (hard pointer) */
            DRW_DBG("first attrib Handle: "); DRW_DBGHL(attH.code, attH.size, attH.ref); DRW_DBG("\n");
            attH = buf->getHandle(); /* H 2 BLOCK HEADER (hard pointer) */
            DRW_DBG("second attrib Handle: "); DRW_DBGHL(attH.code, attH.size, attH.ref); DRW_DBG("\n");
        } else {
            for (duint8 i=0; i< objCount; ++i){
                dwgHandle attH = buf->getHandle(); /* H 2 BLOCK HEADER (hard pointer) */
                DRW_DBG("attrib Handle #"); DRW_DBG(i); DRW_DBG(": "); DRW_DBGHL(attH.code, attH.size, attH.ref); DRW_DBG("\n");
            }
        }
        seqendH = buf->getHandle(); /* H 2 BLOCK HEADER (hard pointer) */
        DRW_DBG("seqendH Handle: "); DRW_DBGHL(seqendH.code, seqendH.size, seqendH.ref); DRW_DBG("\n");
    }
    DRW_DBG("   Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_LWPolyline::applyExtrusion(){
    if (haveExtrusion) {
        calculateAxis(extPoint);
        for (unsigned int i=0; i<vertlist.size(); i++) {
            DRW_Vertex2D *vert = vertlist.at(i);
            DRW_Coord v(vert->x, vert->y, elevation);
            extrudePoint(extPoint, &v);
            vert->x = v.x;
            vert->y = v.y;
        }
    }
}

void DRW_LWPolyline::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 10: {
        vertex = new DRW_Vertex2D();
        vertlist.push_back(vertex);
        vertex->x = reader->getDouble();
        break; }
    case 20:
        if(vertex != NULL)
            vertex->y = reader->getDouble();
        break;
    case 40:
        if(vertex != NULL)
            vertex->stawidth = reader->getDouble();
        break;
    case 41:
        if(vertex != NULL)
            vertex->endwidth = reader->getDouble();
        break;
    case 42:
        if(vertex != NULL)
            vertex->bulge = reader->getDouble();
        break;
    case 38:
        elevation = reader->getDouble();
        break;
    case 39:
        thickness = reader->getDouble();
        break;
    case 43:
        width = reader->getDouble();
        break;
    case 70:
        flags = reader->getInt32();
        break;
    case 90:
        vertexnum = reader->getInt32();
        vertlist.reserve(vertexnum);
        break;
    case 210:
        haveExtrusion = true;
        extPoint.x = reader->getDouble();
        break;
    case 220:
        extPoint.y = reader->getDouble();
        break;
    case 230:
        extPoint.z = reader->getDouble();
        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

bool DRW_LWPolyline::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing LWPolyline *******************************************\n");

    flags = buf->getBitShort();
    DRW_DBG("flags value: "); DRW_DBG(flags);
    if (flags & 4)
        width = buf->getBitDouble();
    if (flags & 8)
        elevation = buf->getBitDouble();
    if (flags & 2)
        thickness = buf->getBitDouble();
    if (flags & 1)
        extPoint = buf->getExtrusion(false);
    vertexnum = buf->getBitLong();
    // 防护：顶点数不可能超过「本对象剩余位数」（每个顶点至少占 1 位）。
    // 对象解析一旦错位，这里会读出一个天文数字（实测 33554432），随后
    // 顶点分配 + 顶点表打印会变成事实上的死循环、把日志撑到 GB 级。
    // 宁可判该对象解析失败，也不要拖垮整个读取流程。
    if (vertexnum < 0
        || static_cast<duint32>(vertexnum) > static_cast<duint32>(buf->numRemainingBytes()) * 8u) {
        DRW_DBG("\nLWPolyline: implausible vertex count "); DRW_DBG(vertexnum);
        DRW_DBG(" (> remaining bits), object rejected\n");
        return false;
    }
    vertlist.reserve(vertexnum);
    unsigned int bulgesnum = 0;
    if (flags & 16)
        bulgesnum = buf->getBitLong();
    int vertexIdCount = 0;
    if (version > DRW::AC1021) {//2010+
        if (flags & 1024)
            vertexIdCount = buf->getBitLong();
    }

    unsigned int widthsnum = 0;
    if (flags & 32)
        widthsnum = buf->getBitLong();
    DRW_DBG("\nvertex num: "); DRW_DBG(vertexnum); DRW_DBG(" bulges num: "); DRW_DBG(bulgesnum);
    DRW_DBG(" vertexIdCount: "); DRW_DBG(vertexIdCount); DRW_DBG(" widths num: "); DRW_DBG(widthsnum);
    //clear all bit except 128 = plinegen and set 1 to open/close //RLZ:verify plinegen & open
    //dxf: plinegen 128 & open 1
    flags = (flags & 512)? (flags | 1):(flags | 0);
    flags &= 129;
    DRW_DBG("end flags value: "); DRW_DBG(flags);

    if (vertexnum > 0) { //verify if is lwpol without vertex (empty)
        // add vertexs
        vertex = new DRW_Vertex2D();
        vertex->x = buf->getRawDouble();
        vertex->y = buf->getRawDouble();
        vertlist.push_back(vertex);
        DRW_Vertex2D* pv = vertex;
        for (int i = 1; i< vertexnum; i++){
            vertex = new DRW_Vertex2D();
            if (version < DRW::AC1015) {//14-
                vertex->x = buf->getRawDouble();
                vertex->y = buf->getRawDouble();
            } else {
//                DRW_Vertex2D *pv = vertlist.back();
                vertex->x = buf->getDefaultDouble(pv->x);
                vertex->y = buf->getDefaultDouble(pv->y);
            }
            pv = vertex;
            vertlist.push_back(vertex);
        }
        //add bulges
        for (unsigned int i = 0; i < bulgesnum; i++){
            double bulge = buf->getBitDouble();
            if (vertlist.size()> i)
                vertlist.at(i)->bulge = bulge;
        }
        //add vertexId
        if (version > DRW::AC1021) {//2010+
            for (int i = 0; i < vertexIdCount; i++){
                dint32 vertexId = buf->getBitLong();
                //TODO implement vertexId, do not exist in dxf
                DRW_UNUSED(vertexId);
//                if (vertlist.size()< i)
//                    vertlist.at(i)->vertexId = vertexId;
            }
        }
        //add widths
        for (unsigned int i = 0; i < widthsnum; i++){
            double staW = buf->getBitDouble();
            double endW = buf->getBitDouble();
            if (vertlist.size()< i) {
                vertlist.at(i)->stawidth = staW;
                vertlist.at(i)->endwidth = endW;
            }
        }
    }
    if (DRW_DBGGL == DRW_dbg::DEBUG){
        DRW_DBG("\nVertex list: ");
        for (std::vector<DRW_Vertex2D *>::iterator it = vertlist.begin() ; it != vertlist.end(); ++it){
            DRW_Vertex2D* pv = *it;
            DRW_DBG("\n   x: "); DRW_DBG(pv->x); DRW_DBG(" y: "); DRW_DBG(pv->y); DRW_DBG(" bulge: "); DRW_DBG(pv->bulge);
            DRW_DBG(" stawidth: "); DRW_DBG(pv->stawidth); DRW_DBG(" endwidth: "); DRW_DBG(pv->endwidth);
        }
    }

    DRW_DBG("\n");
    /* Common Entity Handle Data */
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    /* CRC X --- */
    return buf->isGood();
}


void DRW_Text::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 40:
        height = reader->getDouble();
        break;
    case 41:
        widthscale = reader->getDouble();
        break;
    case 50:
        angle = reader->getDouble();
        break;
    case 51:
        oblique = reader->getDouble();
        break;
    case 71:
        textgen = reader->getInt32();
        break;
    case 72:
        alignH = (HAlign)reader->getInt32();
        break;
    case 73:
        alignV = (VAlign)reader->getInt32();
        break;
    case 1:
        text = reader->getUtf8String();
        break;
    case 7:
        style = reader->getUtf8String();
        break;
    default:
        DRW_Line::parseCode(code, reader);
        break;
    }
}

// ============================================================================
//  ATTDEF / ATTRIB
//
//  DXF 组码：ATTRIB 的 73 是「字段长度」而 74 才是垂直对齐——与 TEXT 的
//  73=垂直对齐**不同义**。因此这两个码必须在子类里截住，不能透传给
//  DRW_Text::parseCode，否则垂直对齐会被字段长度覆盖。
// ============================================================================
void DRW_Attdef::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        tag = reader->getUtf8String();
        break;
    case 3:
        prompt = reader->getUtf8String();
        break;
    case 70:
        flags = reader->getInt32();
        break;
    case 73:                    /* 字段长度：本库不建模，读出即丢 */
        (void)reader->getInt32();
        break;
    case 74:
        alignV = (VAlign)reader->getInt32();
        break;
    default:
        DRW_Text::parseCode(code, reader);
        break;
    }
}

bool DRW_Attdef::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    if (!parseDwgTextData(version, buf, sBuf, bs)) return false;

    // 属性专属尾部
    //   标签 T → 字段长度 BS → 标志 RC → 提示 T
    // 这里有个陷阱：**标志是 RC（1 字节），不是 BS**，而且它前面还有一个字段长度。
    // 少了这两步，句柄流会整体错位、buf->isGood() 为假。实测一张 AutoCAD 2000
    // 图纸：标签 ETIKEDO、标志 0、提示 invitilo、默认值 valoro、插入点
    // (-289.217687, -218.054561)、字高 100 —— 与同一张图的 DXF 逐个对上。
    tag = sBuf->getVariableText(version, false);
    (void)buf->getBitShort();                        /* field_length（73），不建模 */
    flags = buf->getRawChar8();                      /* RC，见上 */
    if (version > DRW::AC1014) {//2000+
        prompt = sBuf->getVariableText(version, false);
    }
    DRW_DBG("ATTDEF tag: "); DRW_DBG(tag.c_str());
    DRW_DBG(", flags: "); DRW_DBG(flags);
    DRW_DBG(", prompt: "); DRW_DBG(prompt.c_str()); DRW_DBG("\n");

    if (!DRW_Entity::parseDwgEntHandle(version, buf)) return false;
    styleH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    return buf->isGood();
}

void DRW_Attrib::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        tag = reader->getUtf8String();
        break;
    case 70:
        flags = reader->getInt32();
        break;
    case 73:                    /* 字段长度：本库不建模，读出即丢 */
        (void)reader->getInt32();
        break;
    case 74:
        alignV = (VAlign)reader->getInt32();
        break;
    default:
        DRW_Text::parseCode(code, reader);
        break;
    }
}

bool DRW_Attrib::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    if (!parseDwgTextData(version, buf, sBuf, bs)) return false;

    // 尾部与 ATTDEF 同构，只少最后那个提示串：
    //   标签 T → 字段长度 BS → 标志 RC
    tag = sBuf->getVariableText(version, false);
    (void)buf->getBitShort();                        /* field_length（73），不建模 */
    flags = buf->getRawChar8();                      /* RC */
    DRW_DBG("ATTRIB tag: "); DRW_DBG(tag.c_str());
    DRW_DBG(", flags: "); DRW_DBG(flags); DRW_DBG("\n");

    if (!DRW_Entity::parseDwgEntHandle(version, buf)) return false;
    styleH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    return buf->isGood();
}

// ---------------------------------------------------------------------------
//  DRW_DimArc（DXF: ARC_DIMENSION / AcDbArcDimension）
//
//  这个实体里有两组同号组码，靠子类标记区分：
//    AcDbDimension 段的 70 = 标注类型、71 = 文字附着点
//    AcDbArcDimension 段的 70 = 子类标志位、71 = 是否有引线
//  因此必须先看到 `100 AcDbArcDimension` 才能按弧长语义解释它们——照抄
//  DRW_Dimension::parseCode 会把子类标志位当成标注类型，类型与文字附着点全错。
// ---------------------------------------------------------------------------
void DRW_DimArc::parseCode(int code, dxfReader *reader){
    if (code == 100) {
        const std::string subclass = reader->getString();
        inArcSubclass = (subclass == "AcDbArcDimension");
        return;
    }

    if (inArcSubclass) {
        switch (code) {
        case 13: xline1Pt.x = reader->getDouble(); return;
        case 23: xline1Pt.y = reader->getDouble(); return;
        case 33: xline1Pt.z = reader->getDouble(); return;
        case 14: xline2Pt.x = reader->getDouble(); return;
        case 24: xline2Pt.y = reader->getDouble(); return;
        case 34: xline2Pt.z = reader->getDouble(); return;
        case 15: centerPt.x = reader->getDouble(); return;
        case 25: centerPt.y = reader->getDouble(); return;
        case 35: centerPt.z = reader->getDouble(); return;
        case 16: leader1Pt.x = reader->getDouble(); return;
        case 26: leader1Pt.y = reader->getDouble(); return;
        case 36: leader1Pt.z = reader->getDouble(); return;
        case 17: leader2Pt.x = reader->getDouble(); return;
        case 27: leader2Pt.y = reader->getDouble(); return;
        case 37: leader2Pt.z = reader->getDouble(); return;
        case 40: arcStartParam = reader->getDouble(); return;   /* 弧度 */
        case 41: arcEndParam = reader->getDouble(); return;     /* 弧度 */
        case 70: subFlags = reader->getInt32(); return;
        case 71: hasLeader = reader->getInt32(); return;
        default: break;
        }
    }
    DRW_Dimension::parseCode(code, reader);
}

bool DRW_DimArc::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    if (!DRW_Entity::parseDwg(version, buf, sBuf, bs)) {
        DRW_DBG("ARC_DIMENSION: 公共实体数据解析失败\n");
        return false;
    }
    if (!DRW_Dimension::parseDwg(version, buf, sBuf)) {
        DRW_DBG("ARC_DIMENSION: DIMENSION 公共段解析失败\n");
        return false;
    }

    // 弧长标注在 DWG 里是**类对象**（CLASSES 段里登记 ARC_DIMENSION），字段序
    //   定义点 3BD → 界线 1 3BD → 界线 2 3BD → 弧心 3BD → is_partial 位
    //   → 起参数 BD → 止参数 BD → has_leader 位 → 引线 1 3BD → 引线 2 3BD
    // 注意定义点（DXF 10 组码）在 DWG 里是放在子类段里的，与其它标注同构。
    setDefPoint(buf->get3BitDouble());
    xline1Pt = buf->get3BitDouble();
    xline2Pt = buf->get3BitDouble();
    centerPt = buf->get3BitDouble();
    subFlags = buf->getBit();
    arcStartParam = buf->getBitDouble();
    arcEndParam = buf->getBitDouble();
    hasLeader = buf->getBit();
    leader1Pt = buf->get3BitDouble();
    leader2Pt = buf->get3BitDouble();

    // DWG 的 70 存的是「非默认位置」等标志位，类型由类对象本身决定；
    // 补上 bit3 让上层与 DXF 路径看到同一个类型值。
    type |= 8;

    if (!DRW_Entity::parseDwgEntHandle(version, buf)) return false;
    dimStyleH = buf->getHandle();   /* H 5 DIMSTYLE */
    blockH = buf->getHandle();      /* H 5 匿名几何块 */
    return buf->isGood();
}

bool DRW_Text::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    if (!parseDwgTextData(version, buf, sBuf, bs)) return false;

    /* Common Entity Handle Data */
    if (!DRW_Entity::parseDwgEntHandle(version, buf)) return false;

    styleH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("text style Handle: "); DRW_DBGHL(styleH.code, styleH.size, styleH.ref); DRW_DBG("\n");

    return buf->isGood();
}

bool DRW_Text::parseDwgTextData(DRW::Version version, dwgBuffer *buf, dwgBuffer *sBuf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing text *********************************************\n");

 // DataFlags RC Used to determine presence of subsquent data, set to 0xFF for R14-
    duint8 data_flags = 0x00;
    if (version > DRW::AC1014) {//2000+
        data_flags = buf->getRawChar8(); /* DataFlags RC Used to determine presence of subsquent data */
        DRW_DBG("data_flags: "); DRW_DBG(data_flags); DRW_DBG("\n");
        if ( !(data_flags & 0x01) ) { /* Elevation RD --- present if !(DataFlags & 0x01) */
            basePoint.z = buf->getRawDouble();
        }
    } else {//14-
        basePoint.z = buf->getBitDouble(); /* Elevation BD --- */
    }
    basePoint.x = buf->getRawDouble(); /* Insertion pt 2RD 10 */
    basePoint.y = buf->getRawDouble();
    DRW_DBG("Insert point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z); DRW_DBG("\n");
    if (version > DRW::AC1014) {//2000+
        if ( !(data_flags & 0x02) ) { /* Alignment pt 2DD 11 present if !(DataFlags & 0x02), use 10 & 20 values for 2 default values.*/
            secPoint.x = buf->getDefaultDouble(basePoint.x);
            secPoint.y = buf->getDefaultDouble(basePoint.y);
        } else {
            secPoint = basePoint;
        }
    } else {//14-
        secPoint.x = buf->getRawDouble();  /* Alignment pt 2RD 11 */
        secPoint.y = buf->getRawDouble();
    }
    secPoint.z = basePoint.z;
    DRW_DBG("Alignment: "); DRW_DBGPT(secPoint.x, secPoint.y, basePoint.z); DRW_DBG("\n");
    extPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG("Extrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z); DRW_DBG("\n");
    thickness = buf->getThickness(version > DRW::AC1014); /* Thickness BD 39 */

    if (version > DRW::AC1014) {//2000+
        if ( !(data_flags & 0x04) ) { /* Oblique ang RD 51 present if !(DataFlags & 0x04) */
            oblique = buf->getRawDouble();
        }
        if ( !(data_flags & 0x08) ) { /* Rotation ang RD 50 present if !(DataFlags & 0x08) */
            angle = buf->getRawDouble();
        }
        height = buf->getRawDouble(); /* Height RD 40 */
        if ( !(data_flags & 0x10) ) { /* Width factor RD 41 present if !(DataFlags & 0x10) */
            widthscale = buf->getRawDouble();
        }
    } else {//14-
        oblique = buf->getBitDouble(); /* Oblique ang BD 51 */
        angle = buf->getBitDouble(); /* Rotation ang BD 50 */
        height = buf->getBitDouble(); /* Height BD 40 */
        widthscale = buf->getBitDouble(); /* Width factor BD 41 */
    }
    DRW_DBG("thickness: "); DRW_DBG(thickness); DRW_DBG(", Oblique ang: "); DRW_DBG(oblique); DRW_DBG(", Width: ");
    DRW_DBG(widthscale); DRW_DBG(", Rotation: "); DRW_DBG(angle); DRW_DBG(", height: "); DRW_DBG(height); DRW_DBG("\n");
    text = sBuf->getVariableText(version, false); /* Text value TV 1 */
    DRW_DBG("text string: "); DRW_DBG(text.c_str());DRW_DBG("\n");
    //textgen, alignH, alignV always present in R14-, data_flags set in initialisation
    if ( !(data_flags & 0x20) ) { /* Generation BS 71 present if !(DataFlags & 0x20) */
        textgen = buf->getBitShort();
        DRW_DBG("textgen: "); DRW_DBG(textgen);
    }
    if ( !(data_flags & 0x40) ) { /* Horiz align. BS 72 present if !(DataFlags & 0x40) */
        alignH = (HAlign)buf->getBitShort();
        DRW_DBG(", alignH: "); DRW_DBG(alignH);
    }
    if ( !(data_flags & 0x80) ) { /* Vert align. BS 73 present if !(DataFlags & 0x80) */
        alignV = (VAlign)buf->getBitShort();
        DRW_DBG(", alignV: "); DRW_DBG(alignV);
    }
    DRW_DBG("\n");

    /* CRC X --- */
    return buf->isGood();
}

void DRW_MText::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 1:
        text += reader->getString();
        text = reader->toUtf8String(text);
        break;
    case 11:
        haveXAxis = true;
        DRW_Text::parseCode(code, reader);
        break;
    case 3:
        text += reader->getString();
        break;
    case 44:
        interlin = reader->getDouble();
        break;
    default:
        DRW_Text::parseCode(code, reader);
        break;
    }
}

bool DRW_MText::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing mtext *********************************************\n");

    basePoint = buf->get3BitDouble(); /* Insertion pt 3BD 10 - First picked point. */
    DRW_DBG("Insertion: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z); DRW_DBG("\n");
    extPoint = buf->get3BitDouble(); /* Extrusion 3BD 210 Undocumented; */
    secPoint = buf->get3BitDouble(); /* X-axis dir 3BD 11 */
    /* DWG 里 11 组码**总是**「X 轴方向向量」，MTEXT 的旋转只存在这一个字段里
    （MTEXT 没有 50 组码）。不置位 haveXAxis，紧随其后的 updateAngle() 就是空操作，
    angle 会一直停在构造时的 0，调用方拿不到任何旋转依据——实测一张 2018 版 DWG
    （AC1032）：ENTITIES 段的 272 条文字里有 202 条是旋转 90° 的 MTEXT，
    旋转全部丢失、被画成水平；同一张图纸的 DXF 因为 parseCode(11) 会置位，全部正确。 */
    haveXAxis = true;
    updateAngle();
    widthscale = buf->getBitDouble(); /* Rect width BD 41 */
    if (version > DRW::AC1018) {//2007+
        /* Rect height BD 46 Reference rectangle height. */
        /** @todo */buf->getBitDouble();
    }
    height = buf->getBitDouble();/* Text height BD 40 Undocumented */
    textgen = buf->getBitShort(); /* Attachment BS 71 Similar to justification; */
    /* Drawing dir BS 72 Left to right, etc.; see DXF doc
    MTEXT 的 72 组码是**书写方向**（不是 TEXT 的横向对齐），与 DXF 路径同义地存进 alignH：
    不读它，TopToBottom（竖排）的 MTEXT 往返后会被写成从左到右。 */
    alignH = (HAlign)buf->getBitShort();
    /* Extents ht BD Undocumented and not present in DXF or entget */
    double ext_ht = buf->getBitDouble();
    DRW_UNUSED(ext_ht);
    /* Extents wid BD Undocumented and not present in DXF or entget The extents
    rectangle, when rotated the same as the text, fits the actual text image on
    the screen (altough we've seen it include an extra row of text in height). */
    double ext_wid = buf->getBitDouble();
    DRW_UNUSED(ext_wid);
    /* Text TV 1 All text in one long string (without '\n's 3 for line wrapping).
    ACAD seems to add braces ({ }) and backslash-P's to indicate paragraphs
    based on the "\r\n"'s found in the imported file. But, all the text is in
    this one long string -- not broken into 1- and 3-groups as in DXF and
    entget. ACAD's entget breaks this string into 250-char pieces (not 255 as
    doc'd) – even if it's mid-word. The 1-group always gets the tag end;
    therefore, the 3's are always 250 chars long. */
    text = sBuf->getVariableText(version, false); /* Text value TV 1 */
    if (version > DRW::AC1014) {//2000+
        /* MTEXT 的 73 = **行距样式**（不是 TEXT 的纵向对齐）、44 = 行距系数，
        与 DXF 路径同义地存进 alignV / interlin：不读它们，往返后「固定行距」
        会被写成「至少」，行距系数也会退回默认 1.0。 */
        alignV = (VAlign)buf->getBitShort(); /* Linespacing Style BS 73 */
        interlin = buf->getBitDouble();      /* Linespacing Factor BD 44 */
        buf->getBit();/* Unknown bit B */
    }
    if (version > DRW::AC1015) {//2004+
        /* Background flags BL 0 = no background, 1 = background fill, 2 =background
        fill with drawing fill color. */
        dint32 bk_flags = buf->getBitLong(); /** @todo add to DRW_MText */
        if ( bk_flags == 1 ) {
            /* Background scale factor BL Present if background flags = 1, default = 1.5*/
            buf->getBitLong();
            /* Background color CMC Present if background flags = 1 */
            buf->getCmColor(version); //RLZ: warning CMC or ENC
            /** @todo buf->getCMC */
            /* Background transparency BL Present if background flags = 1 */
            buf->getBitLong();
        }
    }
    // 2018+：一个「非注释性」位（libredwg dwg.spec 的
    // `SINCE (R_2018) FIELD_B (is_not_annotative, 0)`；为真时其后跟一整块
    // AnnotScaleObject 字段）。本实现按普通多行文字处理。
    // 必须与写出端对称：漏读会让公共句柄段整体错 1 位、文字样式句柄读错。
    if (version >= DRW::AC1032) {//2018+
        duint8 isNotAnnotative = buf->getBit();
        DRW_DBG("\nis_not_annotative: "); DRW_DBG(isNotAnnotative); DRW_DBG("\n");
    }

    /* Common Entity Handle Data */
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;

    styleH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("text style Handle: "); DRW_DBG(styleH.code); DRW_DBG(".");
    DRW_DBG(styleH.size); DRW_DBG("."); DRW_DBG(styleH.ref); DRW_DBG("\n");

    /* CRC X --- */
    return buf->isGood();
}

void DRW_MText::updateAngle(){
    if (haveXAxis) {
            /* 写入的是**度**，与 DRW_Text::angle 声明的单位一致（DXF 的 50 组码单位）。
            注意：这条换算与调用路径无关——DXF 与 DWG 的 MTEXT 由此得到同一个单位，
            但和同一条路径上 TEXT 的 DWG 角度（弧度）并不一致。 */
            angle = atan2(secPoint.y, secPoint.x)*180/M_PI;
    }
}

void DRW_Polyline::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 70:
        flags = reader->getInt32();
        break;
    case 40:
        defstawidth = reader->getDouble();
        break;
    case 41:
        defendwidth = reader->getDouble();
        break;
    case 71:
        vertexcount = reader->getInt32();
        break;
    case 72:
        facecount = reader->getInt32();
        break;
    case 73:
        smoothM = reader->getInt32();
        break;
    case 74:
        smoothN = reader->getInt32();
        break;
    case 75:
        curvetype = reader->getInt32();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

//0x0F polyline 2D bit 4(8) & 5(16) NOT set
//0x10 polyline 3D bit 4(8) set
//0x1D PFACE bit 5(16) set
bool DRW_Polyline::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing polyline *********************************************\n");

    dint32 ooCount = 0;
    if (oType == 0x0F) { //pline 2D
        flags = buf->getBitShort();
        DRW_DBG("flags value: "); DRW_DBG(flags);
        curvetype = buf->getBitShort();
        defstawidth = buf->getBitDouble();
        defendwidth = buf->getBitDouble();
        thickness = buf->getThickness(version > DRW::AC1014);
        basePoint = DRW_Coord(0,0,buf->getBitDouble());
        extPoint = buf->getExtrusion(version > DRW::AC1014);
    } else if (oType == 0x10) { //pline 3D
        duint8 tmpFlag = buf->getRawChar8();
        DRW_DBG("flags 1 value: "); DRW_DBG(tmpFlag);
        if (tmpFlag & 1)
            curvetype = 5;
        else if (tmpFlag & 2)
            curvetype = 6;
        if (tmpFlag & 3) {
            curvetype = 8;
            flags |= 4;
        }
        tmpFlag = buf->getRawChar8();
        if (tmpFlag & 1)
            flags |= 1;
        flags |= 8; //indicate 3DPOL
        DRW_DBG("flags 2 value: "); DRW_DBG(tmpFlag);
    } else if (oType == 0x1D) { //PFACE
        flags = 64;
        vertexcount = buf->getBitShort();
        DRW_DBG("vertex count: "); DRW_DBG(vertexcount);
        facecount = buf->getBitShort();
        DRW_DBG("face count: "); DRW_DBG(facecount);
        DRW_DBG("flags value: "); DRW_DBG(flags);
    }
    if (version > DRW::AC1015){ //2004+
        ooCount = buf->getBitLong();
    }

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;

    if (version < DRW::AC1018){ //2000-
        dwgHandle objectH = buf->getOffsetHandle(handle);
        firstEH = objectH.ref;
        DRW_DBG(" first Vertex Handle: "); DRW_DBGHL(objectH.code, objectH.size, objectH.ref); DRW_DBG("\n");
        objectH = buf->getOffsetHandle(handle);
        lastEH = objectH.ref;
        DRW_DBG(" last Vertex Handle: "); DRW_DBGHL(objectH.code, objectH.size, objectH.ref); DRW_DBG("\n");
        DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    } else {
        for (dint32 i = 0; i < ooCount; ++i){
                dwgHandle objectH = buf->getOffsetHandle(handle);
                hadlesList.push_back (objectH.ref);
                DRW_DBG(" Vertex Handle: "); DRW_DBGHL(objectH.code, objectH.size, objectH.ref); DRW_DBG("\n");
                DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        }
    }
    seqEndH = buf->getOffsetHandle(handle);
    DRW_DBG(" SEQEND Handle: "); DRW_DBGHL(seqEndH.code, seqEndH.size, seqEndH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Vertex::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 70:
        flags = reader->getInt32();
        break;
    case 40:
        stawidth = reader->getDouble();
        break;
    case 41:
        endwidth = reader->getDouble();
        break;
    case 42:
        bulge = reader->getDouble();
        break;
    case 50:
        tgdir = reader->getDouble();
        break;
    case 71:
        vindex1 = reader->getInt32();
        break;
    case 72:
        vindex2 = reader->getInt32();
        break;
    case 73:
        vindex3 = reader->getInt32();
        break;
    case 74:
        vindex4 = reader->getInt32();
        break;
    case 91:
        identifier = reader->getInt32();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

//0x0A vertex 2D
//0x0B vertex 3D
//0x0C MESH
//0x0D PFACE
//0x0E PFACE FACE
bool DRW_Vertex::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs, double el){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing pline Vertex *********************************************\n");

    if (oType == 0x0A) { //pline 2D, needed example
        flags = buf->getRawChar8(); //RLZ: EC  unknown type
        DRW_DBG("flags value: "); DRW_DBG(flags);
        basePoint = buf->get3BitDouble();
        basePoint.z = el;
        DRW_DBG("basePoint: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
        stawidth = buf->getBitDouble();
        if (stawidth < 0)
			endwidth = stawidth = fabs(stawidth);
        else
            endwidth = buf->getBitDouble();
        bulge = buf->getBitDouble();
        if (version > DRW::AC1021) //2010+
            DRW_DBG("Vertex ID: "); DRW_DBG(buf->getBitLong());
        tgdir = buf->getBitDouble();
    } else if (oType == 0x0B || oType == 0x0C || oType == 0x0D) { //PFACE
        flags = buf->getRawChar8(); //RLZ: EC  unknown type
        DRW_DBG("flags value: "); DRW_DBG(flags);
        basePoint = buf->get3BitDouble();
        DRW_DBG("basePoint: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    } else if (oType == 0x0E) { //PFACE FACE
        vindex1 = buf->getBitShort();
        vindex2 = buf->getBitShort();
        vindex3 = buf->getBitShort();
        vindex4 = buf->getBitShort();
    }

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Hatch::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 2:
        name = reader->getUtf8String();
        break;
    case 70:
        solid = reader->getInt32();
        break;
    case 71:
        associative = reader->getInt32();
        break;
    case 72:        /*edge type*/
        if (ispol){ //if is polyline is a as_bulge flag
            break;
        } else if (reader->getInt32() == 1){ //line
            addLine();
        } else if (reader->getInt32() == 2){ //arc
            addArc();
        } else if (reader->getInt32() == 3){ //elliptic arc
            addEllipse();
        } else if (reader->getInt32() == 4){ //spline
            addSpline();
        }
        break;
    case 10:
        if (pt) pt->basePoint.x = reader->getDouble();
        else if (pline) {
            plvert = pline->addVertex();
            plvert->x = reader->getDouble();
        }
        break;
    case 20:
        if (pt) pt->basePoint.y = reader->getDouble();
        else if (plvert) plvert ->y = reader->getDouble();
        break;
    case 11:
        if (line) line->secPoint.x = reader->getDouble();
        else if (ellipse) ellipse->secPoint.x = reader->getDouble();
        break;
    case 21:
        if (line) line->secPoint.y = reader->getDouble();
        else if (ellipse) ellipse->secPoint.y = reader->getDouble();
        break;
    case 40:
        if (arc) arc->radious = reader->getDouble();
        else if (ellipse) ellipse->ratio = reader->getDouble();
        break;
    case 41:
        scale = reader->getDouble();
        break;
    case 42:
        if (plvert) plvert ->bulge = reader->getDouble();
        break;
    case 50:
        if (arc) arc->staangle = reader->getDouble()/ARAD;
        else if (ellipse) ellipse->staparam = reader->getDouble()/ARAD;
        break;
    case 51:
        if (arc) arc->endangle = reader->getDouble()/ARAD;
        else if (ellipse) ellipse->endparam = reader->getDouble()/ARAD;
        break;
    case 52:
        angle = reader->getDouble();
        break;
    case 53:        /*图案定义线：一条线以 53 开始，随后是基点/偏移/虚线*/
        deflist.push_back(DRW_HatchDefLine());
        deflist.back().angle = reader->getDouble()/ARAD;
        break;
    case 43:
        if (!deflist.empty()) deflist.back().base.x = reader->getDouble();
        break;
    case 44:
        if (!deflist.empty()) deflist.back().base.y = reader->getDouble();
        break;
    case 45:
        if (!deflist.empty()) deflist.back().offset.x = reader->getDouble();
        break;
    case 46:
        if (!deflist.empty()) deflist.back().offset.y = reader->getDouble();
        break;
    case 49:
        if (!deflist.empty()) deflist.back().dashes.push_back(reader->getDouble());
        break;
    case 73:
        if (arc) arc->isccw = reader->getInt32();
        else if (pline) pline->flags = reader->getInt32();
        break;
    case 75:
        hstyle = reader->getInt32();
        break;
    case 76:
        hpattern = reader->getInt32();
        break;
    case 77:
        doubleflag = reader->getInt32();
        break;
    case 78:
        deflines = reader->getInt32();
        break;
    case 91:
        loopsnum = reader->getInt32();
        looplist.reserve(loopsnum);
        break;
    case 92:
        loop = new DRW_HatchLoop(reader->getInt32());
        looplist.push_back(loop);
        if (reader->getInt32() & 2) {
            ispol = true;
            clearEntities();
            pline = new DRW_LWPolyline;
            loop->objlist.push_back(pline);
        } else ispol = false;
        break;
    case 93:
        if (pline) pline->vertexnum = reader->getInt32();
        else loop->numedges = reader->getInt32();//aqui reserve
        break;
    case 98: //seed points ??
        clearEntities();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}

bool DRW_Hatch::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    duint32 totalBoundItems = 0;
    bool havePixelSize = false;

    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing hatch *********************************************\n");

    //Gradient data, RLZ: is ok or if grad > 0 continue read ?
    if (version > DRW::AC1015) { //2004+
        dint32 isGradient = buf->getBitLong();
        DRW_DBG("is Gradient: "); DRW_DBG(isGradient);
        dint32 res = buf->getBitLong();
        DRW_DBG(" reserved: "); DRW_DBG(res);
        double gradAngle = buf->getBitDouble();
        DRW_DBG(" Gradient angle: "); DRW_DBG(gradAngle);
        double gradShift = buf->getBitDouble();
        DRW_DBG(" Gradient shift: "); DRW_DBG(gradShift);
        dint32 singleCol = buf->getBitLong();
        DRW_DBG("\nsingle color Grad: "); DRW_DBG(singleCol);
        double gradTint = buf->getBitDouble();
        DRW_DBG(" Gradient tint: "); DRW_DBG(gradTint);
        dint32 numCol = buf->getBitLong();
        DRW_DBG(" num colors: "); DRW_DBG(numCol);
        for (dint32 i = 0 ; i < numCol; ++i){
            double unkDouble = buf->getBitDouble();
            DRW_DBG("\nunkDouble: "); DRW_DBG(unkDouble);
            duint16 unkShort = buf->getBitShort();
            DRW_DBG(" unkShort: "); DRW_DBG(unkShort);
            dint32 rgbCol = buf->getBitLong();
            DRW_DBG(" rgb color: "); DRW_DBG(rgbCol);
            duint8 ignCol = buf->getRawChar8();
            DRW_DBG(" ignored color: "); DRW_DBG(ignCol);
        }
        UTF8STRING gradName = sBuf->getVariableText(version, false);
        DRW_DBG("\ngradient name: "); DRW_DBG(gradName.c_str()); DRW_DBG("\n");
    }
    basePoint.z = buf->getBitDouble();
    extPoint = buf->get3BitDouble();
    DRW_DBG("base point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    DRW_DBG("\nextrusion: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);
    name = sBuf->getVariableText(version, false);
    DRW_DBG("\nhatch pattern name: "); DRW_DBG(name.c_str()); DRW_DBG("\n");
    solid = buf->getBit();
    associative = buf->getBit();
    loopsnum = buf->getBitLong();

    //read loops
    for (dint32 i = 0 ; i < loopsnum; ++i){
        loop = new DRW_HatchLoop(buf->getBitLong());
        havePixelSize |= loop->type & 4;
        if (!(loop->type & 2)){ //Not polyline
            dint32 numPathSeg = buf->getBitLong();
            for (dint32 j = 0; j<numPathSeg;++j){
                duint8 typePath = buf->getRawChar8();
                if (typePath == 1){ //line
                    addLine();
                    line->basePoint = buf->get2RawDouble();
                    line->secPoint = buf->get2RawDouble();
                } else if (typePath == 2){ //circle arc
                    addArc();
                    arc->basePoint = buf->get2RawDouble();
                    arc->radious = buf->getBitDouble();
                    arc->staangle = buf->getBitDouble();
                    arc->endangle = buf->getBitDouble();
                    arc->isccw = buf->getBit();
                } else if (typePath == 3){ //ellipse arc
                    addEllipse();
                    ellipse->basePoint = buf->get2RawDouble();
                    ellipse->secPoint = buf->get2RawDouble();
                    ellipse->ratio = buf->getBitDouble();
                    ellipse->staparam = buf->getBitDouble();
                    ellipse->endparam = buf->getBitDouble();
                    ellipse->isccw = buf->getBit();
                } else if (typePath == 4){ //spline
                    addSpline();
                    spline->degree = buf->getBitLong();
                    bool isRational = buf->getBit();
                    spline->flags |= (isRational << 2); //rational
                    spline->flags |= (buf->getBit() << 1); //periodic
                    // refer the information at
                    // https://www.opendesign.com/files/guestdownloads/OpenDesign_Specification_for_.dwg_files.pdf
                    // first get the number of knots and control points
                    // and then repeat to read the points.
                    // > numknots BL 95 number of knots
                    // > numctlpts BL 96 number of control points
                    spline->nknots = buf->getBitLong();
                    spline->ncontrol = buf->getBitLong();
                    spline->knotslist.reserve(spline->nknots);
                    spline->controllist.reserve(spline->ncontrol);
                    for (dint32 j = 0; j < spline->nknots;++j){
                        spline->knotslist.push_back (buf->getBitDouble());
                    }
                    for (dint32 j = 0; j < spline->ncontrol;++j){
                        // pt0 2RD 10 control point
                        DRW_Coord* crd = new DRW_Coord(buf->get2RawDouble());
                        spline->controllist.push_back(crd);
                        if(isRational)
                            crd->z =  buf->getBitDouble(); //RLZ: investigate how store weight
                        spline->controllist.push_back(crd);
                    }
                    if (version > DRW::AC1021) { //2010+
                        spline->nfit = buf->getBitLong();
                        spline->fitlist.reserve(spline->nfit);
                        for (dint32 j = 0; j < spline->nfit;++j){
                            // Fitpoint 2RD 11
                            DRW_Coord* crd = new DRW_Coord(buf->get2RawDouble());
                            spline->fitlist.push_back (crd);
                        }
                        spline->tgStart = buf->get2RawDouble();
                        spline->tgEnd = buf->get2RawDouble();
                    }
                }
            }
        } else { //end not pline, start polyline
            pline = new DRW_LWPolyline;
            bool asBulge = buf->getBit();
            pline->flags = buf->getBit();//closed bit
            dint32 numVert = buf->getBitLong();
            for (dint32 j = 0; j<numVert;++j){
                DRW_Vertex2D v;
                v.x = buf->getRawDouble();
                v.y = buf->getRawDouble();
                if (asBulge)
                    v.bulge = buf->getBitDouble();
                pline->addVertex(v);
            }
            loop->objlist.push_back(pline);
        }//end polyline
        loop->update();
        looplist.push_back(loop);
        totalBoundItems += buf->getBitLong();
        DRW_DBG(" totalBoundItems: "); DRW_DBG(totalBoundItems);
    } //end read loops

    hstyle = buf->getBitShort();
    hpattern = buf->getBitShort();
    DRW_DBG("\nhatch style: "); DRW_DBG(hstyle); DRW_DBG(" pattern type"); DRW_DBG(hpattern);
    if (!solid){
        angle = buf->getBitDouble();
        scale = buf->getBitDouble();
        doubleflag = buf->getBit();
        deflines = buf->getBitShort();
        for (dint32 i = 0 ; i < deflines; ++i){
            DRW_HatchDefLine defLine;   // 上游读完成员的局部量就扔，这里留下来
            defLine.angle = buf->getBitDouble();
            defLine.base.x = buf->getBitDouble();
            defLine.base.y = buf->getBitDouble();
            defLine.offset.x = buf->getBitDouble();
            defLine.offset.y = buf->getBitDouble();
            duint16 numDashL = buf->getBitShort();
            DRW_DBG("\ndef line: "); DRW_DBG(defLine.angle); DRW_DBG(","); DRW_DBG(defLine.base.x);
            DRW_DBG(","); DRW_DBG(defLine.base.y);
            DRW_DBG(","); DRW_DBG(defLine.offset.x); DRW_DBG(","); DRW_DBG(defLine.offset.y);
            for (duint16 j = 0 ; j < numDashL; ++j){
                const double lenghtL = buf->getBitDouble();
                defLine.dashes.push_back(lenghtL);
                DRW_DBG(","); DRW_DBG(lenghtL);
            }
            deflist.push_back(defLine);
        }//end deflines
    } //end not solid

    if (havePixelSize){
        ddouble64 pixsize = buf->getBitDouble();
        DRW_DBG("\npixel size: "); DRW_DBG(pixsize);
    }
    dint32 numSeedPoints = buf->getBitLong();
    DRW_DBG("\nnum Seed Points  "); DRW_DBG(numSeedPoints);
    //read Seed Points
    DRW_Coord seedPt;
    for (dint32 i = 0 ; i < numSeedPoints; ++i){
        seedPt.x = buf->getRawDouble();
        seedPt.y = buf->getRawDouble();
        DRW_DBG("\n  "); DRW_DBG(seedPt.x); DRW_DBG(","); DRW_DBG(seedPt.y);
    }

    DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    for (duint32 i = 0 ; i < totalBoundItems; ++i){
        dwgHandle biH = buf->getHandle();
        DRW_DBG("Boundary Items Handle: "); DRW_DBGHL(biH.code, biH.size, biH.ref);
    }
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Spline::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 210:
        normalVec.x = reader->getDouble();
        break;
    case 220:
        normalVec.y = reader->getDouble();
        break;
    case 230:
        normalVec.z = reader->getDouble();
        break;
    case 12:
        tgStart.x = reader->getDouble();
        break;
    case 22:
        tgStart.y = reader->getDouble();
        break;
    case 32:
        tgStart.z = reader->getDouble();
        break;
    case 13:
        tgEnd.x = reader->getDouble();
        break;
    case 23:
        tgEnd.y = reader->getDouble();
        break;
    case 33:
        tgEnd.z = reader->getDouble();
        break;
    case 70:
        flags = reader->getInt32();
        break;
    case 71:
        degree = reader->getInt32();
        break;
    case 72:
        nknots = reader->getInt32();
        break;
    case 73:
        ncontrol = reader->getInt32();
        break;
    case 74:
        nfit = reader->getInt32();
        break;
    case 42:
        tolknot = reader->getDouble();
        break;
    case 43:
        tolcontrol = reader->getDouble();
        break;
    case 44:
        tolfit = reader->getDouble();
        break;
    case 10: {
        controlpoint = new DRW_Coord();
        controllist.push_back(controlpoint);
        controlpoint->x = reader->getDouble();
        break; }
    case 20:
        if(controlpoint != NULL)
            controlpoint->y = reader->getDouble();
        break;
    case 30:
        if(controlpoint != NULL)
            controlpoint->z = reader->getDouble();
        break;
    case 11: {
        fitpoint = new DRW_Coord();
        fitlist.push_back(fitpoint);
        fitpoint->x = reader->getDouble();
        break; }
    case 21:
        if(fitpoint != NULL)
            fitpoint->y = reader->getDouble();
        break;
    case 31:
        if(fitpoint != NULL)
            fitpoint->z = reader->getDouble();
        break;
    case 40:
        knotslist.push_back(reader->getDouble());
        break;
//    case 41:
//        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

bool DRW_Spline::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    bool ret = DRW_Entity::parseDwg(version, buf, NULL, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing spline *********************************************\n");
    duint8 weight = 0; // RLZ ??? flags, weight, code 70, bit 4 (16)

    dint32 scenario = buf->getBitLong();
    DRW_DBG("scenario: "); DRW_DBG(scenario);
    if (version > DRW::AC1024) {
        dint32 splFlag1 = buf->getBitLong();
        if (splFlag1 & 1)
            scenario = 2;
        dint32 knotParam = buf->getBitLong();
        DRW_DBG("2013 splFlag1: "); DRW_DBG(splFlag1); DRW_DBG(" 2013 knotParam: ");
        DRW_DBG(knotParam);
//        DRW_DBG("unk bit: "); DRW_DBG(buf->getBit());
    }
    degree = buf->getBitLong(); //RLZ: code 71, verify with dxf
    DRW_DBG(" degree: "); DRW_DBG(degree); DRW_DBG("\n");
    if (scenario == 2) {
        flags = 8;//scenario 2 = not rational & planar
        tolfit = buf->getBitDouble();//BD
        DRW_DBG("flags: "); DRW_DBG(flags); DRW_DBG(" tolfit: "); DRW_DBG(tolfit);
        tgStart =buf->get3BitDouble();
        DRW_DBG(" Start Tangent: "); DRW_DBGPT(tgStart.x, tgStart.y, tgStart.z);
        tgEnd =buf->get3BitDouble();
        DRW_DBG("\nEnd Tangent: "); DRW_DBGPT(tgEnd.x, tgEnd.y, tgEnd.z);
        nfit = buf->getBitLong();
        DRW_DBG("\nnumber of fit points: "); DRW_DBG(nfit);
    } else if (scenario == 1) {
        flags = 8;//scenario 1 = rational & planar
        flags |= buf->getBit() << 2; //flags, rational, code 70, bit 2 (4)
        flags |= buf->getBit(); //flags, closed, code 70, bit 0 (1)
        flags |= buf->getBit() << 1; //flags, periodic, code 70, bit 1 (2)
        tolknot = buf->getBitDouble();
        tolcontrol = buf->getBitDouble();
        DRW_DBG("flags: "); DRW_DBG(flags); DRW_DBG(" knot tolerance: "); DRW_DBG(tolknot);
        DRW_DBG(" control point tolerance: "); DRW_DBG(tolcontrol);
        nknots = buf->getBitLong();
        ncontrol = buf->getBitLong();
        weight = buf->getBit(); // RLZ ??? flags, weight, code 70, bit 4 (16)
        DRW_DBG("\nnum of knots: "); DRW_DBG(nknots); DRW_DBG(" num of control pt: ");
        DRW_DBG(ncontrol); DRW_DBG(" weight bit: "); DRW_DBG(weight);
    } else {
        DRW_DBG("\ndwg Ellipse, unknouwn scenario\n");
        return false; //RLZ: from doc only 1 or 2 are ok ?
    }

    knotslist.reserve(nknots);
    for (dint32 i= 0; i<nknots; ++i){
        knotslist.push_back (buf->getBitDouble());
    }
    controllist.reserve(ncontrol);
    for (dint32 i= 0; i<ncontrol; ++i){
        DRW_Coord* crd = new DRW_Coord(buf->get3BitDouble());
        controllist.push_back(crd);
        if (weight){
            DRW_DBG("\n w: "); DRW_DBG(buf->getBitDouble()); //RLZ Warning: D (BD or RD)
        }
    }
    fitlist.reserve(nfit);
    for (dint32 i= 0; i<nfit; ++i){
        DRW_Coord* crd = new DRW_Coord(buf->get3BitDouble());
        fitlist.push_back (crd);
    }
    if (DRW_DBGGL == DRW_dbg::DEBUG){
        DRW_DBG("\nknots list: ");
        for (std::vector<double>::iterator it = knotslist.begin() ; it != knotslist.end(); ++it){
            DRW_DBG("\n"); DRW_DBG(*it);
        }
        DRW_DBG("\ncontrol point list: ");
        for (std::vector<DRW_Coord *>::iterator it = controllist.begin() ; it != controllist.end(); ++it){
            DRW_DBG("\n"); DRW_DBGPT((*it)->x,(*it)->y,(*it)->z);
        }
        DRW_DBG("\nfit point list: ");
        for (std::vector<DRW_Coord *>::iterator it = fitlist.begin() ; it != fitlist.end(); ++it){
            DRW_DBG("\n"); DRW_DBGPT((*it)->x,(*it)->y,(*it)->z);
        }
    }

    /* Common Entity Handle Data */
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Image::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 12:
        vVector.x = reader->getDouble();
        break;
    case 22:
        vVector.y = reader->getDouble();
        break;
    case 32:
        vVector.z = reader->getDouble();
        break;
    case 13:
        sizeu = reader->getDouble();
        break;
    case 23:
        sizev = reader->getDouble();
        break;
    case 340:
        ref = reader->getHandleString();
        break;
    case 280:
        clip = reader->getInt32();
        break;
    case 281:
        brightness = reader->getInt32();
        break;
    case 282:
        contrast = reader->getInt32();
        break;
    case 283:
        fade = reader->getInt32();
        break;
    default:
        DRW_Line::parseCode(code, reader);
        break;
    }
}

bool DRW_Image::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing image *********************************************\n");

    dint32 classVersion = buf->getBitLong();
    DRW_DBG("class Version: "); DRW_DBG(classVersion);
    basePoint = buf->get3BitDouble();
    DRW_DBG("\nbase point: "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    secPoint = buf->get3BitDouble();
    DRW_DBG("\nU vector: "); DRW_DBGPT(secPoint.x, secPoint.y, secPoint.z);
    vVector = buf->get3BitDouble();
    DRW_DBG("\nV vector: "); DRW_DBGPT(vVector.x, vVector.y, vVector.z);
    sizeu = buf->getRawDouble();
    sizev = buf->getRawDouble();
    DRW_DBG("\nsize U: "); DRW_DBG(sizeu); DRW_DBG("\nsize V: "); DRW_DBG(sizev);
    duint16 displayProps = buf->getBitShort();
    DRW_UNUSED(displayProps);//RLZ: temporary, complete API
    clip = buf->getBit();
    brightness = buf->getRawChar8();
    contrast = buf->getRawChar8();
    fade = buf->getRawChar8();
    if (version > DRW::AC1021){ //2010+
        bool clipMode = buf->getBit();
        DRW_UNUSED(clipMode);//RLZ: temporary, complete API
    }
    duint16 clipType = buf->getBitShort();
    if (clipType == 1){
        buf->get2RawDouble();
        buf->get2RawDouble();
    } else { //clipType == 2
        dint32 numVerts = buf->getBitLong();
        for (int i= 0; i< numVerts;++i)
            buf->get2RawDouble();
    }

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    dwgHandle biH = buf->getHandle();
    DRW_DBG("ImageDef Handle: "); DRW_DBGHL(biH.code, biH.size, biH.ref);
    ref = biH.ref;
    biH = buf->getHandle();
    DRW_DBG("ImageDefReactor Handle: "); DRW_DBGHL(biH.code, biH.size, biH.ref);
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Dimension::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 1:
        text = reader->getUtf8String();
        break;
    case 2:
        name = reader->getString();
        break;
    case 3:
        style = reader->getUtf8String();
        break;
    case 70:
        type = reader->getInt32();
        break;
    case 71:
        align = reader->getInt32();
        break;
    case 72:
        linesty = reader->getInt32();
        break;
    case 10:
        defPoint.x = reader->getDouble();
        break;
    case 20:
        defPoint.y = reader->getDouble();
        break;
    case 30:
        defPoint.z = reader->getDouble();
        break;
    case 11:
        textPoint.x = reader->getDouble();
        break;
    case 21:
        textPoint.y = reader->getDouble();
        break;
    case 31:
        textPoint.z = reader->getDouble();
        break;
    case 12:
        clonePoint.x = reader->getDouble();
        break;
    case 22:
        clonePoint.y = reader->getDouble();
        break;
    case 32:
        clonePoint.z = reader->getDouble();
        break;
    case 13:
        def1.x = reader->getDouble();
        break;
    case 23:
        def1.y = reader->getDouble();
        break;
    case 33:
        def1.z = reader->getDouble();
        break;
    case 14:
        def2.x = reader->getDouble();
        break;
    case 24:
        def2.y = reader->getDouble();
        break;
    case 34:
        def2.z = reader->getDouble();
        break;
    case 15:
        circlePoint.x = reader->getDouble();
        break;
    case 25:
        circlePoint.y = reader->getDouble();
        break;
    case 35:
        circlePoint.z = reader->getDouble();
        break;
    case 16:
        arcPoint.x = reader->getDouble();
        break;
    case 26:
        arcPoint.y = reader->getDouble();
        break;
    case 36:
        arcPoint.z = reader->getDouble();
        break;
    case 41:
        linefactor = reader->getDouble();
        break;
    case 53:
        rot = reader->getDouble();
        break;
    case 50:
        angle = reader->getDouble();
        break;
    case 52:
        oblique = reader->getDouble();
        break;
    case 40:
        length = reader->getDouble();
        break;
    case 51:
        hdir = reader->getDouble();
        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

bool DRW_Dimension::parseDwg(DRW::Version version, dwgBuffer *buf, dwgBuffer *sBuf){
    DRW_DBG("\n***************************** parsing dimension *********************************************");
    if (version > DRW::AC1021) { //2010+
        duint8 dimVersion = buf->getRawChar8();
        DRW_DBG("\ndimVersion: "); DRW_DBG(dimVersion);
    }
    // 标注的拉伸方向在 R13-R15 均为普通 3BD，没有公共实体数据那种「默认标志位」，也没有附加未知位。
    extPoint = buf->get3BitDouble();
    DRW_DBG("\nextPoint: "); DRW_DBGPT(extPoint.x, extPoint.y, extPoint.z);
    textPoint.x = buf->getRawDouble();
    textPoint.y = buf->getRawDouble();
    textPoint.z = buf->getBitDouble();
    DRW_DBG("\ntextPoint: "); DRW_DBGPT(textPoint.x, textPoint.y, textPoint.z);
    type = buf->getRawChar8();
    DRW_DBG("\ntype (70) read: "); DRW_DBG(type);
    type =  (type & 1) ? type & 0x7F : type | 0x80; //set bit 7
    type =  (type & 2) ? type | 0x20 : type & 0xDF; //set bit 5
    DRW_DBG(" type (70) set: "); DRW_DBG(type);
    //clear last 3 bits to set integer dim type
    type &= 0xF8;
    text = sBuf->getVariableText(version, false);
    DRW_DBG("\nforced dim text: "); DRW_DBG(text.c_str());
    rot = buf->getBitDouble();
    hdir = buf->getBitDouble();
    DRW_Coord inspoint = buf->get3BitDouble();
    DRW_DBG("\ninspoint: "); DRW_DBGPT(inspoint.x, inspoint.y, inspoint.z);
    double insRot_code54 = buf->getBitDouble(); //RLZ: unknown, investigate
    DRW_DBG(" insRot_code54: "); DRW_DBG(insRot_code54);
    if (version > DRW::AC1014) { //2000+
        align = buf->getBitShort();
        linesty = buf->getBitShort();
        linefactor = buf->getBitDouble();
        double actMeas = buf->getBitDouble();
        // 42 组码（实测值）此前只读进局部变量就丢了，DRW_Dimension::measurement
        // 永远是默认 0——上层拿不到「图里写的测量值」。坐标标注要靠它判断
        // X 型/Y 型（两条读取路径的标志位不在同一处），所以必须存下来。
        measurement = actMeas;
        DRW_DBG("\n  actMeas_code42: "); DRW_DBG(actMeas);
        if (version > DRW::AC1018) { //2007+
            bool unk = buf->getBit();
            bool flip1 = buf->getBit();
            bool flip2 = buf->getBit();
            DRW_DBG("\n2007, unk, flip1, flip2: "); DRW_DBG(unk); DRW_DBG(flip1); DRW_DBG(flip2);
        }
    }
    clonePoint.x = buf->getRawDouble();
    clonePoint.y = buf->getRawDouble();
    DRW_DBG("\nclonePoint: "); DRW_DBGPT(clonePoint.x, clonePoint.y, clonePoint.z);

    return buf->isGood();
}

bool DRW_DimAligned::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    ret = DRW_Dimension::parseDwg(version, buf, sBuf);
    if (!ret)
        return ret;
    if (oType == 0x15)
        DRW_DBG("\n***************************** parsing dim linear *********************************************\n");
    else
        DRW_DBG("\n***************************** parsing dim aligned *********************************************\n");
    DRW_Coord pt = buf->get3BitDouble();
    setPt3(pt); //def1
    DRW_DBG("def1: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt4(pt);
    DRW_DBG("\ndef2: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setDefPoint(pt);
    DRW_DBG("\ndefPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    setOb52(buf->getBitDouble());
    if (oType == 0x15)
        setAn50(buf->getBitDouble() * ARAD);
    else
        type |= 1;
    DRW_DBG("\n  type (70) final: "); DRW_DBG(type); DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    if (!ret)
        return ret;
    dimStyleH = buf->getHandle();
    DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
    blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    //    RS crc;   //RS */
    return buf->isGood();
 }

 bool DRW_DimRadial::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
     dwgBuffer sBuff = *buf;
     dwgBuffer *sBuf = buf;
     if (version > DRW::AC1018) {//2007+
         sBuf = &sBuff; //separate buffer for strings
     }
     bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
     if (!ret)
         return ret;
     ret = DRW_Dimension::parseDwg(version, buf, sBuf);
     if (!ret)
         return ret;
     DRW_DBG("\n***************************** parsing dim radial *********************************************\n");
     DRW_Coord pt = buf->get3BitDouble();
     setDefPoint(pt); //code 10
     DRW_DBG("defPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
     pt = buf->get3BitDouble();
     setPt5(pt); //center pt  code 15
     DRW_DBG("\ncenter point: "); DRW_DBGPT(pt.x, pt.y, pt.z);
     setRa40(buf->getBitDouble()); //leader length code 40
     DRW_DBG("\nleader length: "); DRW_DBG(getRa40());
     type |= 4;
     DRW_DBG("\n  type (70) final: "); DRW_DBG(type); DRW_DBG("\n");

     ret = DRW_Entity::parseDwgEntHandle(version, buf);
     DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
     if (!ret)
         return ret;
     dimStyleH = buf->getHandle();
     DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
     blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
     DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
     DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

     //    RS crc;   //RS */
     return buf->isGood();
 }

 bool DRW_DimDiametric::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
     dwgBuffer sBuff = *buf;
     dwgBuffer *sBuf = buf;
     if (version > DRW::AC1018) {//2007+
         sBuf = &sBuff; //separate buffer for strings
     }
     bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
     if (!ret)
         return ret;
     ret = DRW_Dimension::parseDwg(version, buf, sBuf);
     if (!ret)
         return ret;
     DRW_DBG("\n***************************** parsing dim diametric *********************************************\n");
     DRW_Coord pt = buf->get3BitDouble();
     setPt5(pt); //center pt  code 15
     DRW_DBG("center point: "); DRW_DBGPT(pt.x, pt.y, pt.z);
     pt = buf->get3BitDouble();
     setDefPoint(pt); //code 10
     DRW_DBG("\ndefPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
     setRa40(buf->getBitDouble()); //leader length code 40
     DRW_DBG("\nleader length: "); DRW_DBG(getRa40());
     type |= 3;
     DRW_DBG("\n  type (70) final: "); DRW_DBG(type); DRW_DBG("\n");

     ret = DRW_Entity::parseDwgEntHandle(version, buf);
     DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
     if (!ret)
         return ret;
     dimStyleH = buf->getHandle();
     DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
     blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
     DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
     DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

     //    RS crc;   //RS */
     return buf->isGood();
 }

bool DRW_DimAngular::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    ret = DRW_Dimension::parseDwg(version, buf, sBuf);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing dim angular *********************************************\n");
    DRW_Coord pt;
    pt.x = buf->getRawDouble();
    pt.y = buf->getRawDouble();
    setPt6(pt); //code 16
    DRW_DBG("arc Point: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt3(pt); //def1  code 13
    DRW_DBG("\ndef1: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt4(pt); //def2  code 14
    DRW_DBG("\ndef2: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt5(pt); //center pt  code 15
    DRW_DBG("\ncenter point: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setDefPoint(pt); //code 10
    DRW_DBG("\ndefPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    type |= 0x02;
    DRW_DBG("\n  type (70) final: "); DRW_DBG(type); DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    if (!ret)
        return ret;
    dimStyleH = buf->getHandle();
    DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
    blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    //    RS crc;   //RS */
    return buf->isGood();
}

bool DRW_DimAngular3p::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    ret = DRW_Dimension::parseDwg(version, buf, sBuf);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing dim angular3p *********************************************\n");
    DRW_Coord pt = buf->get3BitDouble();
    setDefPoint(pt); //code 10
    DRW_DBG("defPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt3(pt); //def1  code 13
    DRW_DBG("\ndef1: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt4(pt); //def2  code 14
    DRW_DBG("\ndef2: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt5(pt); //center pt  code 15
    DRW_DBG("\ncenter point: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    type |= 0x05;
    DRW_DBG("\n  type (70) final: "); DRW_DBG(type); DRW_DBG("\n");

    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    if (!ret)
        return ret;
    dimStyleH = buf->getHandle();
    DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
    blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    //    RS crc;   //RS */
    return buf->isGood();
}

bool DRW_DimOrdinate::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    ret = DRW_Dimension::parseDwg(version, buf, sBuf);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing dim ordinate *********************************************\n");
    DRW_Coord pt = buf->get3BitDouble();
    setDefPoint(pt);
    DRW_DBG("defPoint: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt3(pt); //def1
    DRW_DBG("\ndef1: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    pt = buf->get3BitDouble();
    setPt4(pt);
    DRW_DBG("\ndef2: "); DRW_DBGPT(pt.x, pt.y, pt.z);
    duint8 type2 = buf->getRawChar8();//RLZ: correct this
    DRW_DBG("type2 (70) read: "); DRW_DBG(type2);
    type =  (type2 & 1) ? type | 0x80 : type & 0xBF; //set bit 6
    DRW_DBG(" type (70) set: "); DRW_DBG(type);
    type |= 6;
    DRW_DBG("\n  type (70) final: "); DRW_DBG(type);

    ret = DRW_Entity::parseDwgEntHandle(version, buf); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    if (!ret)
        return ret;
    dimStyleH = buf->getHandle();
    DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
    blockH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("anon block Handle: "); DRW_DBGHL(blockH.code, blockH.size, blockH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    //    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Leader::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 3:
        style = reader->getUtf8String();
        break;
    case 71:
        arrow = reader->getInt32();
        break;
    case 72:
        leadertype = reader->getInt32();
        break;
    case 73:
        flag = reader->getInt32();
        break;
    case 74:
        hookline = reader->getInt32();
        break;
    case 75:
        hookflag = reader->getInt32();
        break;
    case 76:
        vertnum = reader->getInt32();
        break;
    case 77:
        coloruse = reader->getInt32();
        break;
    case 40:
        textheight = reader->getDouble();
        break;
    case 41:
        textwidth = reader->getDouble();
        break;
    case 10: {
        vertexpoint = new DRW_Coord();
        vertexlist.push_back(vertexpoint);
        vertexpoint->x = reader->getDouble();
        break; }
    case 20:
        if(vertexpoint != NULL)
            vertexpoint->y = reader->getDouble();
        break;
    case 30:
        if(vertexpoint != NULL)
            vertexpoint->z = reader->getDouble();
        break;
    case 340:
        annotHandle = reader->getHandleString();
        break;
    case 210:
        extrusionPoint.x = reader->getDouble();
        break;
    case 220:
        extrusionPoint.y = reader->getDouble();
        break;
    case 230:
        extrusionPoint.z = reader->getDouble();
        break;
    case 211:
        horizdir.x = reader->getDouble();
        break;
    case 221:
        horizdir.y = reader->getDouble();
        break;
    case 231:
        horizdir.z = reader->getDouble();
        break;
    case 212:
        offsetblock.x = reader->getDouble();
        break;
    case 222:
        offsetblock.y = reader->getDouble();
        break;
    case 232:
        offsetblock.z = reader->getDouble();
        break;
    case 213:
        offsettext.x = reader->getDouble();
        break;
    case 223:
        offsettext.y = reader->getDouble();
        break;
    case 233:
        offsettext.z = reader->getDouble();
        break;
    default:
        DRW_Entity::parseCode(code, reader);
        break;
    }
}

bool DRW_Leader::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing leader *********************************************\n");
    DRW_DBG("unknown bit "); DRW_DBG(buf->getBit());
    DRW_DBG(" annot type "); DRW_DBG(buf->getBitShort());
    DRW_DBG(" Path type "); DRW_DBG(buf->getBitShort());
    dint32 nPt = buf->getBitLong();
    DRW_DBG(" Num pts "); DRW_DBG(nPt);

    // add vertexs
    for (int i = 0; i< nPt; i++){
        DRW_Coord* vertex = new DRW_Coord(buf->get3BitDouble());
        vertexlist.push_back(vertex);
        DRW_DBG("\nvertex "); DRW_DBGPT(vertex->x, vertex->y, vertex->z);
    }
    DRW_Coord Endptproj = buf->get3BitDouble();
    DRW_DBG("\nEndptproj "); DRW_DBGPT(Endptproj.x, Endptproj.y, Endptproj.z);
    extrusionPoint = buf->getExtrusion(version > DRW::AC1014);
    DRW_DBG("\nextrusionPoint "); DRW_DBGPT(extrusionPoint.x, extrusionPoint.y, extrusionPoint.z);
    if (version > DRW::AC1014) { //2000+
        DRW_DBG("\nFive unknown bits: "); DRW_DBG(buf->getBit()); DRW_DBG(buf->getBit());
        DRW_DBG(buf->getBit()); DRW_DBG(buf->getBit()); DRW_DBG(buf->getBit());
    }
    horizdir = buf->get3BitDouble();
    DRW_DBG("\nhorizdir "); DRW_DBGPT(horizdir.x, horizdir.y, horizdir.z);
    offsetblock = buf->get3BitDouble();
    DRW_DBG("\noffsetblock "); DRW_DBGPT(offsetblock.x, offsetblock.y, offsetblock.z);
    if (version > DRW::AC1012) { //R14+
        DRW_Coord unk = buf->get3BitDouble();
        DRW_DBG("\nunknown "); DRW_DBGPT(unk.x, unk.y, unk.z);
    }
    if (version < DRW::AC1015) { //R14 -
        DRW_DBG("\ndimgap "); DRW_DBG(buf->getBitDouble());
    }
    if (version < DRW::AC1024) { //2010-
        textheight = buf->getBitDouble();
        textwidth = buf->getBitDouble();
        DRW_DBG("\ntextheight "); DRW_DBG(textheight); DRW_DBG(" textwidth "); DRW_DBG(textwidth);
    }
    hookline = buf->getBit();
    arrow = buf->getBit();
    DRW_DBG(" hookline "); DRW_DBG(hookline); DRW_DBG(" arrow flag "); DRW_DBG(arrow);

    if (version < DRW::AC1015) { //R14 -
        DRW_DBG("\nArrow head type "); DRW_DBG(buf->getBitShort());
        DRW_DBG("dimasz "); DRW_DBG(buf->getBitDouble());
        DRW_DBG("\nunk bit "); DRW_DBG(buf->getBit());
        DRW_DBG(" unk bit "); DRW_DBG(buf->getBit());
        DRW_DBG(" unk short "); DRW_DBG(buf->getBitShort());
        DRW_DBG(" byBlock color "); DRW_DBG(buf->getBitShort());
        DRW_DBG(" unk bit "); DRW_DBG(buf->getBit());
        DRW_DBG(" unk bit "); DRW_DBG(buf->getBit());
    } else { //R2000+
        DRW_DBG("\nunk short "); DRW_DBG(buf->getBitShort());
        DRW_DBG(" unk bit "); DRW_DBG(buf->getBit());
        DRW_DBG(" unk bit "); DRW_DBG(buf->getBit());
    }
    DRW_DBG("\n");
    ret = DRW_Entity::parseDwgEntHandle(version, buf);
    if (!ret)
        return ret;
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
    AnnotH = buf->getHandle();
    annotHandle = AnnotH.ref;
    DRW_DBG("annot block Handle: "); DRW_DBGHL(AnnotH.code, AnnotH.size, dimStyleH.ref); DRW_DBG("\n");
    dimStyleH = buf->getHandle(); /* H 7 STYLE (hard pointer) */
    DRW_DBG("dim style Handle: "); DRW_DBGHL(dimStyleH.code, dimStyleH.size, dimStyleH.ref); DRW_DBG("\n");
    DRW_DBG("Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
//    RS crc;   //RS */
    return buf->isGood();
}

void DRW_Viewport::parseCode(int code, dxfReader *reader){
    switch (code) {
    case 40:
        pswidth = reader->getDouble();
        break;
    case 41:
        psheight = reader->getDouble();
        break;
    case 68:
        vpstatus = reader->getInt32();
        break;
    case 69:
        vpID = reader->getInt32();
        break;
    case 12: {
        centerPX = reader->getDouble();
        break; }
    case 22:
        centerPY = reader->getDouble();
        break;
    default:
        DRW_Point::parseCode(code, reader);
        break;
    }
}
//ex 22 dec 34
bool DRW_Viewport::parseDwg(DRW::Version version, dwgBuffer *buf, duint32 bs){
    dwgBuffer sBuff = *buf;
    dwgBuffer *sBuf = buf;
    if (version > DRW::AC1018) {//2007+
        sBuf = &sBuff; //separate buffer for strings
    }
    bool ret = DRW_Entity::parseDwg(version, buf, sBuf, bs);
    if (!ret)
        return ret;
    DRW_DBG("\n***************************** parsing viewport *****************************************\n");
    basePoint.x = buf->getBitDouble();
    basePoint.y = buf->getBitDouble();
    basePoint.z = buf->getBitDouble();
    DRW_DBG("center "); DRW_DBGPT(basePoint.x, basePoint.y, basePoint.z);
    pswidth = buf->getBitDouble();
    psheight = buf->getBitDouble();
    DRW_DBG("\nWidth: "); DRW_DBG(pswidth); DRW_DBG(", Height: "); DRW_DBG(psheight); DRW_DBG("\n");
    //RLZ TODO: complete in dxf
    if (version > DRW::AC1014) {//2000+
        viewTarget.x = buf->getBitDouble();
        viewTarget.y = buf->getBitDouble();
        viewTarget.z = buf->getBitDouble();
        DRW_DBG("view Target "); DRW_DBGPT(viewTarget.x, viewTarget.y, viewTarget.z);
        viewDir.x = buf->getBitDouble();
        viewDir.y = buf->getBitDouble();
        viewDir.z = buf->getBitDouble();
        DRW_DBG("\nview direction "); DRW_DBGPT(viewDir.x, viewDir.y, viewDir.z);
        twistAngle = buf->getBitDouble();
        DRW_DBG("\nView twist Angle: "); DRW_DBG(twistAngle);
        viewHeight = buf->getBitDouble();
        DRW_DBG("\nview Height: "); DRW_DBG(viewHeight);
        viewLength = buf->getBitDouble();
        DRW_DBG(" Lens Length: "); DRW_DBG(viewLength);
        frontClip = buf->getBitDouble();
        DRW_DBG("\nfront Clip Z: "); DRW_DBG(frontClip);
        backClip = buf->getBitDouble();
        DRW_DBG(" back Clip Z: "); DRW_DBG(backClip);
        snapAngle = buf->getBitDouble();
        DRW_DBG("\n snap Angle: "); DRW_DBG(snapAngle);
        centerPX = buf->getRawDouble();
        centerPY = buf->getRawDouble();
        DRW_DBG("\nview center X: "); DRW_DBG(centerPX); DRW_DBG(", Y: "); DRW_DBG(centerPX);
        snapPX = buf->getRawDouble();
        snapPY = buf->getRawDouble();
        DRW_DBG("\nSnap base point X: "); DRW_DBG(snapPX); DRW_DBG(", Y: "); DRW_DBG(snapPY);
        snapSpPX = buf->getRawDouble();
        snapSpPY = buf->getRawDouble();
        DRW_DBG("\nSnap spacing X: "); DRW_DBG(snapSpPX); DRW_DBG(", Y: "); DRW_DBG(snapSpPY);
        //RLZ: need to complete
        DRW_DBG("\nGrid spacing X: "); DRW_DBG(buf->getRawDouble()); DRW_DBG(", Y: "); DRW_DBG(buf->getRawDouble());DRW_DBG("\n");
        DRW_DBG("Circle zoom?: "); DRW_DBG(buf->getBitShort()); DRW_DBG("\n");
    }
    if (version > DRW::AC1018) {//2007+
        DRW_DBG("Grid major?: "); DRW_DBG(buf->getBitShort()); DRW_DBG("\n");
    }
    if (version > DRW::AC1014) {//2000+
        frozenLyCount = buf->getBitLong();
        DRW_DBG("Frozen Layer count?: "); DRW_DBG(frozenLyCount); DRW_DBG("\n");
        DRW_DBG("Status Flags?: "); DRW_DBG(buf->getBitLong()); DRW_DBG("\n");
        //RLZ: Warning needed separate string bufer
        DRW_DBG("Style sheet?: "); DRW_DBG(sBuf->getVariableText(version, false)); DRW_DBG("\n");
        DRW_DBG("Render mode?: "); DRW_DBG(buf->getRawChar8()); DRW_DBG("\n");
        DRW_DBG("UCS OMore...: "); DRW_DBG(buf->getBit()); DRW_DBG("\n");
        DRW_DBG("UCS VMore...: "); DRW_DBG(buf->getBit()); DRW_DBG("\n");
        DRW_DBG("UCS OMore...: "); DRW_DBGPT(buf->getBitDouble(), buf->getBitDouble(), buf->getBitDouble()); DRW_DBG("\n");
        DRW_DBG("ucs XAMore...: "); DRW_DBGPT(buf->getBitDouble(), buf->getBitDouble(), buf->getBitDouble()); DRW_DBG("\n");
        DRW_DBG("UCS YMore....: "); DRW_DBGPT(buf->getBitDouble(), buf->getBitDouble(), buf->getBitDouble()); DRW_DBG("\n");
        DRW_DBG("UCS EMore...: "); DRW_DBG(buf->getBitDouble()); DRW_DBG("\n");
        DRW_DBG("UCS OVMore...: "); DRW_DBG(buf->getBitShort()); DRW_DBG("\n");
    }
    if (version > DRW::AC1015) {//2004+
        DRW_DBG("ShadePlot Mode...: "); DRW_DBG(buf->getBitShort()); DRW_DBG("\n");
    }
    if (version > DRW::AC1018) {//2007+
        DRW_DBG("Use def Ligth...: "); DRW_DBG(buf->getBit()); DRW_DBG("\n");
        DRW_DBG("Def ligth tipe?: "); DRW_DBG(buf->getRawChar8()); DRW_DBG("\n");
        DRW_DBG("Brightness: "); DRW_DBG(buf->getBitDouble()); DRW_DBG("\n");
        DRW_DBG("Contrast: "); DRW_DBG(buf->getBitDouble()); DRW_DBG("\n");
//        DRW_DBG("Ambient Cmc or Enc: "); DRW_DBG(buf->getCmColor(version)); DRW_DBG("\n");
        DRW_DBG("Ambient (Cmc or Enc?), Enc: "); DRW_DBG(buf->getEnColor(version)); DRW_DBG("\n");
    }
    ret = DRW_Entity::parseDwgEntHandle(version, buf);

    dwgHandle someHdl;
    if (version < DRW::AC1015) {//R13 & R14 only
        DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        someHdl = buf->getHandle();
        DRW_DBG("ViewPort ent header: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
    }
    if (version > DRW::AC1014) {//2000+
        for (duint8 i=0; i < frozenLyCount; ++i){
            someHdl = buf->getHandle();
            DRW_DBG("Frozen layer handle "); DRW_DBG(i); DRW_DBG(": "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        }
        someHdl = buf->getHandle();
        DRW_DBG("Clip bpundary handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        if (version == DRW::AC1015) {//2000 only
            someHdl = buf->getHandle();
            DRW_DBG("ViewPort ent header: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        }
        someHdl = buf->getHandle();
        DRW_DBG("Named ucs handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        someHdl = buf->getHandle();
        DRW_DBG("base ucs handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
    }
    if (version > DRW::AC1018) {//2007+
        someHdl = buf->getHandle();
        DRW_DBG("background handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        someHdl = buf->getHandle();
        DRW_DBG("visual style handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        someHdl = buf->getHandle();
        DRW_DBG("shadeplot ID handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
        DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");
        someHdl = buf->getHandle();
        DRW_DBG("SUN handle: "); DRW_DBGHL(someHdl.code, someHdl.size, someHdl.ref); DRW_DBG("\n");
    }
    DRW_DBG("\n Remaining bytes: "); DRW_DBG(buf->numRemainingBytes()); DRW_DBG("\n");

    if (!ret)
        return ret;
    return buf->isGood();
}
