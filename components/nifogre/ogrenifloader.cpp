/*
  OpenMW - The completely unofficial reimplementation of Morrowind
  Copyright (C) 2008-2010  Nicolay Korslund
  Email: < korslund@gmail.com >
  WWW: http://openmw.sourceforge.net/

  This file (ogre_nif_loader.cpp) is part of the OpenMW package.

  OpenMW is distributed as free software: you can redistribute it
  and/or modify it under the terms of the GNU General Public License
  version 3, as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  version 3 along with this program. If not, see
  http://www.gnu.org/licenses/ .

 */

#include "ogrenifloader.hpp"

#include <algorithm>

#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreHardwareBufferManager.h>
#include <OgreSkeletonManager.h>
#include <OgreTechnique.h>
#include <OgreSubMesh.h>
#include <OgreRoot.h>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreTagPoint.h>
#include <OgreParticleSystem.h>
#include <OgreParticleEmitter.h>
#include <OgreParticleAffector.h>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/functional/hash.hpp>

#include <extern/shiny/Main/Factory.hpp>

#include <components/nif/node.hpp>
#include <components/misc/stringops.hpp>
#include <components/settings/settings.hpp>
#include <components/nifoverrides/nifoverrides.hpp>

typedef unsigned char ubyte;

namespace std
{

// TODO: Do something useful
ostream& operator<<(ostream &o, const NifOgre::TextKeyMap&)
{ return o; }

}

namespace NifOgre
{

// FIXME: Should not be here.
class VisController
{
public:
    class Value : public Ogre::ControllerValue<Ogre::Real>
    {
    private:
        Ogre::Bone *mTarget;

        // FIXME: We are not getting all objects here. Skinned meshes get
        // attached to the object's root node, and won't be connected via a
        // TagPoint.
        static void setVisible(Ogre::Node *node, int vis)
        {
            Ogre::Node::ChildNodeIterator iter = node->getChildIterator();
            while(iter.hasMoreElements())
            {
                node = iter.getNext();
                setVisible(node, vis);

                Ogre::TagPoint *tag = dynamic_cast<Ogre::TagPoint*>(node);
                if(tag != NULL)
                {
                    Ogre::MovableObject *obj = tag->getChildObject();
                    if(obj != NULL)
                        obj->setVisible(vis);
                }
            }
        }

    public:
        Value(Ogre::Bone *target) : mTarget(target)
        { }

        virtual Ogre::Real getValue() const
        {
            // Should not be called
            return 1.0f;
        }

        virtual void setValue(Ogre::Real value)
        {
            int vis = static_cast<int>(value);
            setVisible(mTarget, vis);
        }
    };

    class Function : public Ogre::ControllerFunction<Ogre::Real>
    {
    private:
        std::vector<Nif::NiVisData::VisData> mData;

    public:
        Function(const Nif::NiVisData *data)
          : Ogre::ControllerFunction<Ogre::Real>(false),
            mData(data->mVis)
        { }

        virtual Ogre::Real calculate(Ogre::Real value)
        {
            if(mData.size() == 0)
                return 1.0f;

            if(mData[0].time >= value)
                return mData[0].isSet;
            for(size_t i = 1;i < mData.size();i++)
            {
                if(mData[i].time > value)
                    return mData[i-1].isSet;
            }
            return mData.back().isSet;
        }
    };
};


// Helper class that computes the bounding box and of a mesh
class BoundsFinder
{
    struct MaxMinFinder
    {
        float max, min;

        MaxMinFinder()
        {
            min = std::numeric_limits<float>::infinity();
            max = -min;
        }

        void add(float f)
        {
            if (f > max) max = f;
            if (f < min) min = f;
        }

        // Return Max(max**2, min**2)
        float getMaxSquared()
        {
            float m1 = max*max;
            float m2 = min*min;
            if (m1 >= m2) return m1;
            return m2;
        }
    };

    MaxMinFinder X, Y, Z;

public:
    // Add 'verts' vertices to the calculation. The 'data' pointer is
    // expected to point to 3*verts floats representing x,y,z for each
    // point.
    void add(float *data, int verts)
    {
        for (int i=0;i<verts;i++)
        {
            X.add(*(data++));
            Y.add(*(data++));
            Z.add(*(data++));
        }
    }

    // True if this structure has valid values
    bool isValid()
    {
        return
            minX() <= maxX() &&
            minY() <= maxY() &&
            minZ() <= maxZ();
    }

    // Compute radius
    float getRadius()
    {
        assert(isValid());

        // The radius is computed from the origin, not from the geometric
        // center of the mesh.
        return sqrt(X.getMaxSquared() + Y.getMaxSquared() + Z.getMaxSquared());
    }

    float minX() {
        return X.min;
    }
    float maxX() {
        return X.max;
    }
    float minY() {
        return Y.min;
    }
    float maxY() {
        return Y.max;
    }
    float minZ() {
        return Z.min;
    }
    float maxZ() {
        return Z.max;
    }
};


/** Manual resource loader for NIF skeletons. This is the main class
    responsible for translating the internal NIF skeleton structure into
    something Ogre can use (includes animations and node TextKeyData).
 */
class NIFSkeletonLoader : public Ogre::ManualResourceLoader
{
static void warn(const std::string &msg)
{
    std::cerr << "NIFSkeletonLoader: Warn: " << msg << std::endl;
}

static void fail(const std::string &msg)
{
    std::cerr << "NIFSkeletonLoader: Fail: "<< msg << std::endl;
    abort();
}


static void buildAnimation(Ogre::Skeleton *skel, const std::string &name, const std::vector<const Nif::NiKeyframeController*> &ctrls, const std::vector<std::string> &targets, float startTime, float stopTime)
{
    Ogre::Animation *anim = skel->createAnimation(name, stopTime);

    for(size_t i = 0;i < ctrls.size();i++)
    {
        const Nif::NiKeyframeController *kfc = ctrls[i];
        if(kfc->data.empty())
            continue;
        const Nif::NiKeyframeData *kf = kfc->data.getPtr();

        /* Get the keyframes and make sure they're sorted first to last */
        const Nif::QuaternionKeyList &quatkeys = kf->mRotations;
        const Nif::Vector3KeyList &trankeys = kf->mTranslations;
        const Nif::FloatKeyList &scalekeys = kf->mScales;

        Nif::QuaternionKeyList::VecType::const_iterator quatiter = quatkeys.mKeys.begin();
        Nif::Vector3KeyList::VecType::const_iterator traniter = trankeys.mKeys.begin();
        Nif::FloatKeyList::VecType::const_iterator scaleiter = scalekeys.mKeys.begin();

        Ogre::Bone *bone = skel->getBone(targets[i]);
        // NOTE: For some reason, Ogre doesn't like the node track ID being different from
        // the bone ID
        Ogre::NodeAnimationTrack *nodetrack = anim->hasNodeTrack(bone->getHandle()) ?
                                              anim->getNodeTrack(bone->getHandle()) :
                                              anim->createNodeTrack(bone->getHandle(), bone);

        Ogre::Quaternion lastquat, curquat;
        Ogre::Vector3 lasttrans(0.0f), curtrans(0.0f);
        Ogre::Vector3 lastscale(1.0f), curscale(1.0f);
        if(quatiter != quatkeys.mKeys.end())
            lastquat = curquat = quatiter->mValue;
        if(traniter != trankeys.mKeys.end())
            lasttrans = curtrans = traniter->mValue;
        if(scaleiter != scalekeys.mKeys.end())
            lastscale = curscale = Ogre::Vector3(scaleiter->mValue);
        float begTime = std::max(kfc->timeStart, startTime);
        float endTime = std::min(kfc->timeStop, stopTime);
        bool didlast = false;

        while(!didlast)
        {
            float curtime = std::numeric_limits<float>::max();

            //Get latest time
            if(quatiter != quatkeys.mKeys.end())
                curtime = std::min(curtime, quatiter->mTime);
            if(traniter != trankeys.mKeys.end())
                curtime = std::min(curtime, traniter->mTime);
            if(scaleiter != scalekeys.mKeys.end())
                curtime = std::min(curtime, scaleiter->mTime);

            curtime = std::max(curtime, begTime);
            if(curtime >= endTime)
            {
                didlast = true;
                curtime = endTime;
            }

            // Get the latest quaternions, translations, and scales for the
            // current time
            while(quatiter != quatkeys.mKeys.end() && curtime >= quatiter->mTime)
            {
                lastquat = curquat;
                if(++quatiter != quatkeys.mKeys.end())
                    curquat = quatiter->mValue;
            }
            while(traniter != trankeys.mKeys.end() && curtime >= traniter->mTime)
            {
                lasttrans = curtrans;
                if(++traniter != trankeys.mKeys.end())
                    curtrans = traniter->mValue;
            }
            while(scaleiter != scalekeys.mKeys.end() && curtime >= scaleiter->mTime)
            {
                lastscale = curscale;
                if(++scaleiter != scalekeys.mKeys.end())
                    curscale = Ogre::Vector3(scaleiter->mValue);
            }

            Ogre::TransformKeyFrame *kframe;
            kframe = nodetrack->createNodeKeyFrame(curtime);
            if(quatiter == quatkeys.mKeys.end() || quatiter == quatkeys.mKeys.begin())
                kframe->setRotation(curquat);
            else
            {
                Nif::QuaternionKeyList::VecType::const_iterator last = quatiter-1;
                float diff = (curtime-last->mTime) / (quatiter->mTime-last->mTime);
                kframe->setRotation(Ogre::Quaternion::nlerp(diff, lastquat, curquat));
            }
            if(traniter == trankeys.mKeys.end() || traniter == trankeys.mKeys.begin())
                kframe->setTranslate(curtrans);
            else
            {
                Nif::Vector3KeyList::VecType::const_iterator last = traniter-1;
                float diff = (curtime-last->mTime) / (traniter->mTime-last->mTime);
                kframe->setTranslate(lasttrans + ((curtrans-lasttrans)*diff));
            }
            if(scaleiter == scalekeys.mKeys.end() || scaleiter == scalekeys.mKeys.begin())
                kframe->setScale(curscale);
            else
            {
                Nif::FloatKeyList::VecType::const_iterator last = scaleiter-1;
                float diff = (curtime-last->mTime) / (scaleiter->mTime-last->mTime);
                kframe->setScale(lastscale + ((curscale-lastscale)*diff));
            }
        }
    }
    anim->optimise();
}


static TextKeyMap extractTextKeys(const Nif::NiTextKeyExtraData *tk)
{
    TextKeyMap textkeys;
    for(size_t i = 0;i < tk->list.size();i++)
    {
        const std::string &str = tk->list[i].text;
        std::string::size_type pos = 0;
        while(pos < str.length())
        {
            if(::isspace(str[pos]))
            {
                pos++;
                continue;
            }

            std::string::size_type nextpos = std::min(str.find('\r', pos), str.find('\n', pos));
            std::string result = str.substr(pos, nextpos-pos);
            textkeys.insert(std::make_pair(tk->list[i].time, Misc::StringUtils::toLower(result)));

            pos = nextpos;
        }
    }
    return textkeys;
}

void buildBones(Ogre::Skeleton *skel, const Nif::Node *node, Ogre::Bone *&animroot, TextKeyMap &textkeys, std::vector<Nif::NiKeyframeController const*> &ctrls, Ogre::Bone *parent=NULL)
{
    Ogre::Bone *bone;
    if(!skel->hasBone(node->name))
        bone = skel->createBone(node->name);
    else
        bone = skel->createBone();
    if(parent) parent->addChild(bone);
    mNifToOgreHandleMap[node->recIndex] = bone->getHandle();

    bone->setOrientation(node->trafo.rotation);
    bone->setPosition(node->trafo.pos);
    bone->setScale(Ogre::Vector3(node->trafo.scale));
    bone->setBindingPose();

    if(!(node->recType == Nif::RC_NiNode || /* Nothing special; children traversed below */
         node->recType == Nif::RC_RootCollisionNode || /* handled in nifbullet (hopefully) */
         node->recType == Nif::RC_NiTriShape || /* Handled in the mesh loader */
         node->recType == Nif::RC_NiAutoNormalParticles ||
         node->recType == Nif::RC_NiRotatingParticles
         ))
        warn("Unhandled "+node->recName+" "+node->name+" in "+skel->getName());

    Nif::ControllerPtr ctrl = node->controller;
    while(!ctrl.empty())
    {
        if(ctrl->recType == Nif::RC_NiKeyframeController)
            ctrls.push_back(static_cast<const Nif::NiKeyframeController*>(ctrl.getPtr()));
        else if(!(ctrl->recType == Nif::RC_NiParticleSystemController ||
                  ctrl->recType == Nif::RC_NiVisController
                  ))
            warn("Unhandled "+ctrl->recName+" from node "+node->name+" in "+skel->getName());
        ctrl = ctrl->next;
    }

    Nif::ExtraPtr e = node->extra;
    while(!e.empty())
    {
        if(e->recType == Nif::RC_NiTextKeyExtraData && !animroot)
        {
            const Nif::NiTextKeyExtraData *tk = static_cast<const Nif::NiTextKeyExtraData*>(e.getPtr());
            textkeys = extractTextKeys(tk);
            animroot = bone;
        }
        e = e->extra;
    }

    const Nif::NiNode *ninode = dynamic_cast<const Nif::NiNode*>(node);
    if(ninode)
    {
        const Nif::NodeList &children = ninode->children;
        for(size_t i = 0;i < children.length();i++)
        {
            if(!children[i].empty())
                buildBones(skel, children[i].getPtr(), animroot, textkeys, ctrls, bone);
        }
    }
}

// Lookup to retrieve an Ogre bone handle for a given Nif record index
std::map<int,int> mNifToOgreHandleMap;

typedef std::map<std::string,NIFSkeletonLoader> LoaderMap;
static LoaderMap sLoaders;

public:
void loadResource(Ogre::Resource *resource)
{
    Ogre::Skeleton *skel = dynamic_cast<Ogre::Skeleton*>(resource);
    OgreAssert(skel, "Attempting to load a skeleton into a non-skeleton resource!");

    Nif::NIFFile::ptr nif(Nif::NIFFile::create(skel->getName()));
    const Nif::Node *node = static_cast<const Nif::Node*>(nif->getRecord(0));

    std::vector<const Nif::NiKeyframeController*> ctrls;
    Ogre::Bone *animroot = NULL;
    TextKeyMap textkeys;
    try {
        buildBones(skel, node, animroot, textkeys, ctrls);
    }
    catch(std::exception &e) {
        std::cerr<< "Exception while loading "<<skel->getName() <<std::endl;
        std::cerr<< e.what() <<std::endl;
        return;
    }

    /* Animations without textkeys don't get Ogre::Animation objects. */
    if(!animroot)
        return;

    std::vector<std::string> targets;
    // TODO: If ctrls.size() == 0, check for a .kf file sharing the name of the .nif file
    if(ctrls.size() == 0) // No animations? Then we're done.
        return;

    float maxtime = 0.0f;
    for(size_t i = 0;i < ctrls.size();i++)
    {
        const Nif::NiKeyframeController *ctrl = ctrls[i];
        maxtime = std::max(maxtime, ctrl->timeStop);
        Nif::Named *target = dynamic_cast<Nif::Named*>(ctrl->target.getPtr());
        if(target != NULL)
            targets.push_back(target->name);
    }

    if(targets.size() != ctrls.size())
    {
        warn("Target size mismatch ("+Ogre::StringConverter::toString(targets.size())+" targets, "+
             Ogre::StringConverter::toString(ctrls.size())+" controllers)");
        return;
    }

    Ogre::UserObjectBindings &bindings = animroot->getUserObjectBindings();
    bindings.setUserAny(sTextKeyExtraDataID, Ogre::Any(true));

    std::string currentgroup;
    TextKeyMap::const_iterator keyiter = textkeys.begin();
    for(keyiter = textkeys.begin();keyiter != textkeys.end();keyiter++)
    {
        std::string::size_type sep = keyiter->second.find(':');
        if((sep == currentgroup.length() && keyiter->second.compare(0, sep, currentgroup) == 0) ||
           (sep == sizeof("soundgen")-1 && keyiter->second.compare(0, sep, "soundgen") == 0) ||
           (sep == sizeof("sound")-1 && keyiter->second.compare(0, sep, "sound") == 0))
            continue;
        currentgroup = keyiter->second.substr(0, sep);

        if(skel->hasAnimation(currentgroup))
            continue;

        TextKeyMap::const_iterator lastkeyiter = textkeys.end();
        while((--lastkeyiter)->first > keyiter->first)
        {
            if(lastkeyiter->second.find(':') == currentgroup.length() &&
               lastkeyiter->second.compare(0, currentgroup.length(), currentgroup) == 0)
                break;
        }

        buildAnimation(skel, currentgroup, ctrls, targets, keyiter->first, lastkeyiter->first);

        TextKeyMap::const_iterator insiter(keyiter);
        TextKeyMap groupkeys;
        do {
            sep = insiter->second.find(':');
            if(sep == currentgroup.length() && insiter->second.compare(0, sep, currentgroup) == 0)
                groupkeys.insert(std::make_pair(insiter->first, insiter->second.substr(sep+2)));
            else if((sep == sizeof("soundgen")-1 && insiter->second.compare(0, sep, "soundgen") == 0) ||
                    (sep == sizeof("sound")-1 && insiter->second.compare(0, sep, "sound") == 0))
                groupkeys.insert(std::make_pair(insiter->first, insiter->second));
        } while(insiter++ != lastkeyiter);

        bindings.setUserAny(std::string(sTextKeyExtraDataID)+"@"+currentgroup, Ogre::Any(groupkeys));
    }
}


static Ogre::SkeletonPtr createSkeleton(const std::string &name, const std::string &group, const Nif::Node *node)
{
    /* We need to be a little aggressive here, since some NIFs have a crap-ton
     * of nodes and Ogre only supports 256 bones. We will skip a skeleton if:
     * There are no bones used for skinning, there are no controllers on non-
     * NiTriShape nodes, there are no nodes named "AttachLight", and the tree
     * consists of NiNode, NiTriShape, and RootCollisionNode types only.
     */
    if(!node->boneTrafo)
    {
        if(node->recType == Nif::RC_NiTriShape)
            return Ogre::SkeletonPtr();
        if(node->controller.empty() && node->name != "AttachLight")
        {
            if(node->recType == Nif::RC_NiNode || node->recType == Nif::RC_RootCollisionNode)
            {
                const Nif::NiNode *ninode = static_cast<const Nif::NiNode*>(node);
                const Nif::NodeList &children = ninode->children;
                for(size_t i = 0;i < children.length();i++)
                {
                    if(!children[i].empty())
                    {
                        Ogre::SkeletonPtr skel = createSkeleton(name, group, children[i].getPtr());
                        if(!skel.isNull())
                            return skel;
                    }
                }
                return Ogre::SkeletonPtr();
            }
        }
    }

    Ogre::SkeletonManager &skelMgr = Ogre::SkeletonManager::getSingleton();
    return skelMgr.create(name, group, true, &sLoaders[name]);
}

// Looks up an Ogre Bone handle ID from a NIF's record index. Should only be
// used when the bone name is insufficient as this is a relatively slow lookup
static int lookupOgreBoneHandle(const std::string &nifname, int idx)
{
    LoaderMap::const_iterator loader = sLoaders.find(nifname);
    if(loader != sLoaders.end())
    {
        std::map<int,int>::const_iterator entry = loader->second.mNifToOgreHandleMap.find(idx);
        if(entry != loader->second.mNifToOgreHandleMap.end())
            return entry->second;
    }
    throw std::runtime_error("Invalid NIF record lookup ("+nifname+", index "+Ogre::StringConverter::toString(idx)+")");
}

};
NIFSkeletonLoader::LoaderMap NIFSkeletonLoader::sLoaders;


// Conversion of blend / test mode from NIF
static const char *getBlendFactor(int mode)
{
    switch(mode)
    {
    case 0: return "one";
    case 1: return "zero";
    case 2: return "src_colour";
    case 3: return "one_minus_src_colour";
    case 4: return "dest_colour";
    case 5: return "one_minus_dest_colour";
    case 6: return "src_alpha";
    case 7: return "one_minus_src_alpha";
    case 8: return "dest_alpha";
    case 9: return "one_minus_dest_alpha";
    case 10: return "src_alpha_saturate";
    }
    std::cerr<< "Unexpected blend mode: "<<mode <<std::endl;
    return "src_alpha";
}

static const char *getTestMode(int mode)
{
    switch(mode)
    {
    case 0: return "always_pass";
    case 1: return "less";
    case 2: return "equal";
    case 3: return "less_equal";
    case 4: return "greater";
    case 5: return "not_equal";
    case 6: return "greater_equal";
    case 7: return "always_fail";
    }
    std::cerr<< "Unexpected test mode: "<<mode <<std::endl;
    return "less_equal";
}


class NIFMaterialLoader {

static std::map<size_t,std::string> MaterialMap;

static void warn(const std::string &msg)
{
    std::cerr << "NIFMaterialLoader: Warn: " << msg << std::endl;
}

static void fail(const std::string &msg)
{
    std::cerr << "NIFMaterialLoader: Fail: "<< msg << std::endl;
    abort();
}


static std::string findTextureName(const std::string &filename)
{
    /* Bethesda at some point converted all their BSA
     * textures from tga to dds for increased load speed, but all
     * texture file name references were kept as .tga.
     */
    static const char path[] = "textures\\";
    static const char path2[] = "textures/";


    std::string texname = filename;
    Misc::StringUtils::toLower(texname);

    if(texname.compare(0, sizeof(path)-1, path) != 0
            && texname.compare(0, sizeof(path2)-1, path2) != 0)
        texname = path + texname;

    Ogre::String::size_type pos = texname.rfind('.');
    if(pos != Ogre::String::npos && texname.compare(pos, texname.length() - pos, ".dds") != 0)
    {
        // since we know all (GOTY edition or less) textures end
        // in .dds, we change the extension
        texname.replace(pos, texname.length(), ".dds");

        // if it turns out that the above wasn't true in all cases (not for vanilla, but maybe mods)
        // verify, and revert if false (this call succeeds quickly, but fails slowly)
        if(!Ogre::ResourceGroupManager::getSingleton().resourceExistsInAnyGroup(texname))
        {
            texname = filename;
            Misc::StringUtils::toLower(texname);
            if(texname.compare(0, sizeof(path)-1, path) != 0
                    && texname.compare(0, sizeof(path2)-1, path2) != 0)
                texname = path + texname;
        }
    }

    return texname;
}

public:
static Ogre::String getMaterial(const Nif::ShapeData *shapedata,
                                const Ogre::String &name, const Ogre::String &group,
                                const Nif::NiTexturingProperty *texprop,
                                const Nif::NiMaterialProperty *matprop,
                                const Nif::NiAlphaProperty *alphaprop,
                                const Nif::NiVertexColorProperty *vertprop,
                                const Nif::NiZBufferProperty *zprop,
                                const Nif::NiSpecularProperty *specprop,
                                bool &needTangents)
{
    Ogre::MaterialManager &matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr material = matMgr.getByName(name);
    if(!material.isNull())
        return name;

    Ogre::Vector3 ambient(1.0f);
    Ogre::Vector3 diffuse(1.0f);
    Ogre::Vector3 specular(0.0f);
    Ogre::Vector3 emissive(0.0f);
    float glossiness = 0.0f;
    float alpha = 1.0f;
    int alphaFlags = 0;
    int alphaTest = 0;
    int vertMode = 2;
    //int lightMode = 1;
    int depthFlags = 3;
    // Default should be 1, but Bloodmoon's models are broken
    int specFlags = 0;
    Ogre::String texName[7];

    bool vertexColour = (shapedata->colors.size() != 0);

    // Texture
    if(texprop)
    {
        for(int i = 0;i < 7;i++)
        {
            if(!texprop->textures[i].inUse)
                continue;
            if(texprop->textures[i].texture.empty())
            {
                warn("Texture layer "+Ogre::StringConverter::toString(i)+" is in use but empty in "+name);
                continue;
            }

            const Nif::NiSourceTexture *st = texprop->textures[i].texture.getPtr();
            if(st->external)
                texName[i] = findTextureName(st->filename);
            else
                warn("Found internal texture, ignoring.");
        }
    }
    needTangents = !texName[Nif::NiTexturingProperty::BumpTexture].empty();

    // Alpha modifiers
    if(alphaprop)
    {
        alphaFlags = alphaprop->flags;
        alphaTest = alphaprop->data.threshold;
    }

    // Vertex color handling
    if(vertprop)
    {
        vertMode = vertprop->data.vertmode;
        // FIXME: Handle lightmode?
        //lightMode = vertprop->data.lightmode;
    }

    if(zprop)
    {
        depthFlags = zprop->flags;
        // Depth function???
    }

    if(specprop)
    {
        specFlags = specprop->flags;
    }

    // Material
    if(matprop)
    {
        ambient = matprop->data.ambient;
        diffuse = matprop->data.diffuse;
        specular = matprop->data.specular;
        emissive = matprop->data.emissive;
        glossiness = matprop->data.glossiness;
        alpha = matprop->data.alpha;
    }

    {
        // Generate a hash out of all properties that can affect the material.
        size_t h = 0;
        boost::hash_combine(h, ambient.x);
        boost::hash_combine(h, ambient.y);
        boost::hash_combine(h, ambient.z);
        boost::hash_combine(h, diffuse.x);
        boost::hash_combine(h, diffuse.y);
        boost::hash_combine(h, diffuse.z);
        boost::hash_combine(h, specular.x);
        boost::hash_combine(h, specular.y);
        boost::hash_combine(h, specular.z);
        boost::hash_combine(h, emissive.x);
        boost::hash_combine(h, emissive.y);
        boost::hash_combine(h, emissive.z);
        for(int i = 0;i < 7;i++)
        {
            if(!texName[i].empty())
                boost::hash_combine(h, texName[i]);
        }
        boost::hash_combine(h, vertexColour);
        boost::hash_combine(h, alphaFlags);
        boost::hash_combine(h, alphaTest);
        boost::hash_combine(h, vertMode);
        boost::hash_combine(h, depthFlags);
        boost::hash_combine(h, specFlags);

        std::map<size_t,std::string>::iterator itr = MaterialMap.find(h);
        if (itr != MaterialMap.end())
        {
            // a suitable material exists already - use it
            return itr->second;
        }
        // not found, create a new one
        MaterialMap.insert(std::make_pair(h, name));
    }

    // No existing material like this. Create a new one.
    sh::MaterialInstance *instance = sh::Factory::getInstance().createMaterialInstance(name, "openmw_objects_base");
    if(vertMode == 0 || !vertexColour)
    {
        instance->setProperty("ambient", sh::makeProperty(new sh::Vector4(ambient.x, ambient.y, ambient.z, 1)));
        instance->setProperty("diffuse", sh::makeProperty(new sh::Vector4(diffuse.x, diffuse.y, diffuse.z, alpha)));
        instance->setProperty("emissive", sh::makeProperty(new sh::Vector4(emissive.x, emissive.y, emissive.z, 1)));
        instance->setProperty("vertmode", sh::makeProperty(new sh::StringValue("0")));
    }
    else if(vertMode == 1)
    {
        instance->setProperty("ambient", sh::makeProperty(new sh::Vector4(ambient.x, ambient.y, ambient.z, 1)));
        instance->setProperty("diffuse", sh::makeProperty(new sh::Vector4(diffuse.x, diffuse.y, diffuse.z, alpha)));
        instance->setProperty("emissive", sh::makeProperty(new sh::StringValue("vertexcolour")));
        instance->setProperty("vertmode", sh::makeProperty(new sh::StringValue("1")));
    }
    else if(vertMode == 2)
    {
        instance->setProperty("ambient", sh::makeProperty(new sh::StringValue("vertexcolour")));
        instance->setProperty("diffuse", sh::makeProperty(new sh::StringValue("vertexcolour")));
        instance->setProperty("emissive", sh::makeProperty(new sh::Vector4(emissive.x, emissive.y, emissive.z, 1)));
        instance->setProperty("vertmode", sh::makeProperty(new sh::StringValue("2")));
    }
    else
        std::cerr<< "Unhandled vertex mode: "<<vertMode <<std::endl;

    if(specFlags)
    {
        instance->setProperty("specular", sh::makeProperty(
            new sh::Vector4(specular.x, specular.y, specular.z, glossiness)));
    }

    instance->setProperty("diffuseMap", sh::makeProperty(texName[Nif::NiTexturingProperty::BaseTexture]));
    instance->setProperty("normalMap", sh::makeProperty(texName[Nif::NiTexturingProperty::BumpTexture]));
    instance->setProperty("emissiveMap", sh::makeProperty(texName[Nif::NiTexturingProperty::GlowTexture]));
    if (!texName[Nif::NiTexturingProperty::GlowTexture].empty())
    {
        instance->setProperty("use_emissive_map", sh::makeProperty(new sh::BooleanValue(true)));
        instance->setProperty("emissiveMapUVSet", sh::makeProperty(new sh::IntValue(texprop->textures[Nif::NiTexturingProperty::GlowTexture].uvSet)));
    }

    for(int i = 0;i < 7;i++)
    {
        if(i == Nif::NiTexturingProperty::BaseTexture ||
           i == Nif::NiTexturingProperty::BumpTexture ||
           i == Nif::NiTexturingProperty::GlowTexture)
            continue;
        if(!texName[i].empty())
            warn("Ignored texture "+texName[i]+" on layer "+Ogre::StringConverter::toString(i));
    }

    if (vertexColour)
        instance->setProperty("has_vertex_colour", sh::makeProperty(new sh::BooleanValue(true)));

    // Add transparency if NiAlphaProperty was present
    NifOverrides::TransparencyResult result = NifOverrides::Overrides::getTransparencyOverride(texName[0]);
    if (result.first)
    {
        alphaFlags = (1<<9) | (6<<10); /* alpha_rejection enabled, greater_equal */
        alphaTest = result.second;
        depthFlags = (1<<0) | (1<<1); // depth_write on, depth_check on
    }

    if((alphaFlags&1))
    {
        std::string blend_mode;
        blend_mode += getBlendFactor((alphaFlags>>1)&0xf);
        blend_mode += " ";
        blend_mode += getBlendFactor((alphaFlags>>5)&0xf);
        instance->setProperty("scene_blend", sh::makeProperty(new sh::StringValue(blend_mode)));
    }

    if((alphaFlags>>9)&1)
    {
        std::string reject;
        reject += getTestMode((alphaFlags>>10)&0x7);
        reject += " ";
        reject += Ogre::StringConverter::toString(alphaTest);
        instance->setProperty("alpha_rejection", sh::makeProperty(new sh::StringValue(reject)));
    }
    else
        instance->getMaterial()->setShadowCasterMaterial("openmw_shadowcaster_noalpha");

    // Ogre usually only sorts if depth write is disabled, so we want "force" instead of "on"
    instance->setProperty("transparent_sorting", sh::makeProperty(new sh::StringValue(
        ((alphaFlags&1) && !((alphaFlags>>13)&1)) ? "force" : "off")));

    instance->setProperty("depth_check", sh::makeProperty(new sh::StringValue((depthFlags&1) ? "on" : "off")));
    instance->setProperty("depth_write", sh::makeProperty(new sh::StringValue(((depthFlags>>1)&1) ? "on" : "off")));
    // depth_func???

    sh::Factory::getInstance()._ensureMaterial(name, "Default");
    return name;
}

};
std::map<size_t,std::string> NIFMaterialLoader::MaterialMap;


/** Manual resource loader for NIF meshes. This is the main class
    responsible for translating the internal NIF mesh structure into
    something Ogre can use.
 */
class NIFMeshLoader : Ogre::ManualResourceLoader
{
    std::string mName;
    std::string mGroup;
    size_t mShapeIndex;

    static void warn(const std::string &msg)
    {
        std::cerr << "NIFMeshLoader: Warn: " << msg << std::endl;
    }

    static void fail(const std::string &msg)
    {
        std::cerr << "NIFMeshLoader: Fail: "<< msg << std::endl;
        abort();
    }

    static void getNodeProperties(const Nif::Node *node,
                                  const Nif::NiTexturingProperty *&texprop,
                                  const Nif::NiMaterialProperty *&matprop,
                                  const Nif::NiAlphaProperty *&alphaprop,
                                  const Nif::NiVertexColorProperty *&vertprop,
                                  const Nif::NiZBufferProperty *&zprop,
                                  const Nif::NiSpecularProperty *&specprop)
    {
        if(node->parent)
            getNodeProperties(node->parent, texprop, matprop, alphaprop, vertprop, zprop, specprop);

        const Nif::PropertyList &proplist = node->props;
        for(size_t i = 0;i < proplist.length();i++)
        {
            // Entries may be empty
            if(proplist[i].empty())
                continue;

            const Nif::Property *pr = proplist[i].getPtr();
            if(pr->recType == Nif::RC_NiTexturingProperty)
                texprop = static_cast<const Nif::NiTexturingProperty*>(pr);
            else if(pr->recType == Nif::RC_NiMaterialProperty)
                matprop = static_cast<const Nif::NiMaterialProperty*>(pr);
            else if(pr->recType == Nif::RC_NiAlphaProperty)
                alphaprop = static_cast<const Nif::NiAlphaProperty*>(pr);
            else if(pr->recType == Nif::RC_NiVertexColorProperty)
                vertprop = static_cast<const Nif::NiVertexColorProperty*>(pr);
            else if(pr->recType == Nif::RC_NiZBufferProperty)
                zprop = static_cast<const Nif::NiZBufferProperty*>(pr);
            else if(pr->recType == Nif::RC_NiSpecularProperty)
                specprop = static_cast<const Nif::NiSpecularProperty*>(pr);
            else
                warn("Unhandled property type: "+pr->recName);
        }
    }

    // Convert NiTriShape to Ogre::SubMesh
    void handleNiTriShape(Ogre::Mesh *mesh, Nif::NiTriShape const *shape)
    {
        Ogre::SkeletonPtr skel;
        const Nif::NiTriShapeData *data = shape->data.getPtr();
        const Nif::NiSkinInstance *skin = (shape->skin.empty() ? NULL : shape->skin.getPtr());
        std::vector<Ogre::Vector3> srcVerts = data->vertices;
        std::vector<Ogre::Vector3> srcNorms = data->normals;
        Ogre::HardwareBuffer::Usage vertUsage = Ogre::HardwareBuffer::HBU_STATIC;
        bool vertShadowBuffer = false;
        if(skin != NULL)
        {
            vertUsage = Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY;
            vertShadowBuffer = true;

            // Only set a skeleton when skinning. Unskinned meshes with a skeleton will be
            // explicitly attached later.
            mesh->setSkeletonName(mName);

            // Get the skeleton resource, so vertices can be transformed into the bones' initial state.
            Ogre::SkeletonManager *skelMgr = Ogre::SkeletonManager::getSingletonPtr();
            skel = skelMgr->getByName(mName);

            // Convert vertices and normals to bone space from bind position. It would be
            // better to transform the bones into bind position, but there doesn't seem to
            // be a reliable way to do that.
            std::vector<Ogre::Vector3> newVerts(srcVerts.size(), Ogre::Vector3(0.0f));
            std::vector<Ogre::Vector3> newNorms(srcNorms.size(), Ogre::Vector3(0.0f));

            const Nif::NiSkinData *data = skin->data.getPtr();
            const Nif::NodeList &bones = skin->bones;
            for(size_t b = 0;b < bones.length();b++)
            {
                Ogre::Bone *bone = skel->getBone(bones[b]->name);
                Ogre::Matrix4 mat;
                mat.makeTransform(data->bones[b].trafo.trans, Ogre::Vector3(data->bones[b].trafo.scale),
                                  Ogre::Quaternion(data->bones[b].trafo.rotation));
                mat = bone->_getFullTransform() * mat;

                const std::vector<Nif::NiSkinData::VertWeight> &weights = data->bones[b].weights;
                for(size_t i = 0;i < weights.size();i++)
                {
                    size_t index = weights[i].vertex;
                    float weight = weights[i].weight;

                    newVerts.at(index) += (mat*srcVerts[index]) * weight;
                    if(newNorms.size() > index)
                    {
                        Ogre::Vector4 vec4(srcNorms[index][0], srcNorms[index][1], srcNorms[index][2], 0.0f);
                        vec4 = mat*vec4 * weight;
                        newNorms[index] += Ogre::Vector3(&vec4[0]);
                    }
                }
            }

            srcVerts = newVerts;
            srcNorms = newNorms;
        }
        else
        {
            Ogre::SkeletonManager *skelMgr = Ogre::SkeletonManager::getSingletonPtr();
            if(skelMgr->getByName(mName).isNull())
            {
                // No skinning and no skeleton, so just transform the vertices and
                // normals into position.
                Ogre::Matrix4 mat4 = shape->getWorldTransform();
                for(size_t i = 0;i < srcVerts.size();i++)
                {
                    Ogre::Vector4 vec4(srcVerts[i].x, srcVerts[i].y, srcVerts[i].z, 1.0f);
                    vec4 = mat4*vec4;
                    srcVerts[i] = Ogre::Vector3(&vec4[0]);
                }
                for(size_t i = 0;i < srcNorms.size();i++)
                {
                    Ogre::Vector4 vec4(srcNorms[i].x, srcNorms[i].y, srcNorms[i].z, 0.0f);
                    vec4 = mat4*vec4;
                    srcNorms[i] = Ogre::Vector3(&vec4[0]);
                }
            }
        }

        // Set the bounding box first
        BoundsFinder bounds;
        bounds.add(&srcVerts[0][0], srcVerts.size());
        if(!bounds.isValid())
        {
            float v[3] = { 0.0f, 0.0f, 0.0f };
            bounds.add(&v[0], 1);
        }

        mesh->_setBounds(Ogre::AxisAlignedBox(bounds.minX()-0.5f, bounds.minY()-0.5f, bounds.minZ()-0.5f,
                                              bounds.maxX()+0.5f, bounds.maxY()+0.5f, bounds.maxZ()+0.5f));
        mesh->_setBoundingSphereRadius(bounds.getRadius());

        // This function is just one long stream of Ogre-barf, but it works
        // great.
        Ogre::HardwareBufferManager *hwBufMgr = Ogre::HardwareBufferManager::getSingletonPtr();
        Ogre::HardwareVertexBufferSharedPtr vbuf;
        Ogre::HardwareIndexBufferSharedPtr ibuf;
        Ogre::VertexBufferBinding *bind;
        Ogre::VertexDeclaration *decl;
        int nextBuf = 0;

        Ogre::SubMesh *sub = mesh->createSubMesh();

        // Add vertices
        sub->useSharedVertices = false;
        sub->vertexData = new Ogre::VertexData();
        sub->vertexData->vertexStart = 0;
        sub->vertexData->vertexCount = srcVerts.size();

        decl = sub->vertexData->vertexDeclaration;
        bind = sub->vertexData->vertexBufferBinding;
        if(srcVerts.size())
        {
            vbuf = hwBufMgr->createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3),
                                                srcVerts.size(), vertUsage, vertShadowBuffer);
            vbuf->writeData(0, vbuf->getSizeInBytes(), &srcVerts[0][0], true);

            decl->addElement(nextBuf, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
            bind->setBinding(nextBuf++, vbuf);
        }

        // Vertex normals
        if(srcNorms.size())
        {
            vbuf = hwBufMgr->createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3),
                                                srcNorms.size(), vertUsage, vertShadowBuffer);
            vbuf->writeData(0, vbuf->getSizeInBytes(), &srcNorms[0][0], true);

            decl->addElement(nextBuf, 0, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
            bind->setBinding(nextBuf++, vbuf);
        }

        // Vertex colors
        const std::vector<Ogre::Vector4> &colors = data->colors;
        if(colors.size())
        {
            Ogre::RenderSystem* rs = Ogre::Root::getSingleton().getRenderSystem();
            std::vector<Ogre::RGBA> colorsRGB(colors.size());
            for(size_t i = 0;i < colorsRGB.size();i++)
            {
                Ogre::ColourValue clr(colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
                rs->convertColourValue(clr, &colorsRGB[i]);
            }
            vbuf = hwBufMgr->createVertexBuffer(Ogre::VertexElement::getTypeSize(Ogre::VET_COLOUR),
                                                colorsRGB.size(), Ogre::HardwareBuffer::HBU_STATIC);
            vbuf->writeData(0, vbuf->getSizeInBytes(), &colorsRGB[0], true);
            decl->addElement(nextBuf, 0, Ogre::VET_COLOUR, Ogre::VES_DIFFUSE);
            bind->setBinding(nextBuf++, vbuf);
        }

        // Texture UV coordinates
        size_t numUVs = data->uvlist.size();
        if(numUVs)
        {
            size_t elemSize = Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2);
            vbuf = hwBufMgr->createVertexBuffer(elemSize, srcVerts.size()*numUVs,
                                                Ogre::HardwareBuffer::HBU_STATIC);
            for(size_t i = 0;i < numUVs;i++)
            {
                const std::vector<Ogre::Vector2> &uvlist = data->uvlist[i];
                vbuf->writeData(i*srcVerts.size()*elemSize, elemSize*srcVerts.size(), &uvlist[0], true);
                decl->addElement(nextBuf, i*srcVerts.size()*elemSize, Ogre::VET_FLOAT2,
                                 Ogre::VES_TEXTURE_COORDINATES, i);
            }
            bind->setBinding(nextBuf++, vbuf);
        }

        // Triangle faces
        const std::vector<short> &srcIdx = data->triangles;
        if(srcIdx.size())
        {
            ibuf = hwBufMgr->createIndexBuffer(Ogre::HardwareIndexBuffer::IT_16BIT, srcIdx.size(),
                                               Ogre::HardwareBuffer::HBU_STATIC);
            ibuf->writeData(0, ibuf->getSizeInBytes(), &srcIdx[0], true);
            sub->indexData->indexBuffer = ibuf;
            sub->indexData->indexCount = srcIdx.size();
            sub->indexData->indexStart = 0;
        }

        // Assign bone weights for this TriShape
        if(skin != NULL)
        {
            const Nif::NiSkinData *data = skin->data.getPtr();
            const Nif::NodeList &bones = skin->bones;
            for(size_t i = 0;i < bones.length();i++)
            {
                Ogre::VertexBoneAssignment boneInf;
                boneInf.boneIndex = skel->getBone(bones[i]->name)->getHandle();

                const std::vector<Nif::NiSkinData::VertWeight> &weights = data->bones[i].weights;
                for(size_t j = 0;j < weights.size();j++)
                {
                    boneInf.vertexIndex = weights[j].vertex;
                    boneInf.weight = weights[j].weight;
                    sub->addBoneAssignment(boneInf);
                }
            }
        }

        const Nif::NiTexturingProperty *texprop = NULL;
        const Nif::NiMaterialProperty *matprop = NULL;
        const Nif::NiAlphaProperty *alphaprop = NULL;
        const Nif::NiVertexColorProperty *vertprop = NULL;
        const Nif::NiZBufferProperty *zprop = NULL;
        const Nif::NiSpecularProperty *specprop = NULL;
        bool needTangents = false;

        getNodeProperties(shape, texprop, matprop, alphaprop, vertprop, zprop, specprop);
        std::string matname = NIFMaterialLoader::getMaterial(data, mesh->getName(), mGroup,
                                                             texprop, matprop, alphaprop,
                                                             vertprop, zprop, specprop,
                                                             needTangents);
        if(matname.length() > 0)
            sub->setMaterialName(matname);

        // build tangents if the material needs them
        if (needTangents)
        {
            unsigned short src,dest;
            if (!mesh->suggestTangentVectorBuildParams(Ogre::VES_TANGENT, src,dest))
                mesh->buildTangentVectors(Ogre::VES_TANGENT, src,dest);
        }
    }

    bool findTriShape(Ogre::Mesh *mesh, const Nif::Node *node)
    {
        if(node->recType == Nif::RC_NiTriShape && mShapeIndex == node->recIndex)
        {
            handleNiTriShape(mesh, dynamic_cast<const Nif::NiTriShape*>(node));
            return true;
        }

        const Nif::NiNode *ninode = dynamic_cast<const Nif::NiNode*>(node);
        if(ninode)
        {
            const Nif::NodeList &children = ninode->children;
            for(size_t i = 0;i < children.length();i++)
            {
                if(!children[i].empty())
                {
                    if(findTriShape(mesh, children[i].getPtr()))
                        return true;
                }
            }
        }
        return false;
    }


    typedef std::map<std::string,NIFMeshLoader> LoaderMap;
    static LoaderMap sLoaders;


    static void createParticleEmitterAffectors(Ogre::ParticleSystem *partsys, const Nif::NiParticleSystemController *partctrl)
    {
        Ogre::ParticleEmitter *emitter = partsys->addEmitter("Point");
        emitter->setDirection(Ogre::Vector3(0.0f, 0.0f, std::cos(partctrl->verticalDir)));
        emitter->setAngle(Ogre::Radian(partctrl->verticalAngle));
        emitter->setParticleVelocity(partctrl->velocity-partctrl->velocityRandom,
                                     partctrl->velocity+partctrl->velocityRandom);
        emitter->setEmissionRate(partctrl->emitRate);
        emitter->setTimeToLive(partctrl->lifetime-partctrl->lifetimeRandom,
                               partctrl->lifetime+partctrl->lifetimeRandom);

        Nif::ExtraPtr e = partctrl->extra;
        while(!e.empty())
        {
            if(e->recType == Nif::RC_NiParticleGrowFade)
            {
                const Nif::NiParticleGrowFade *gf = static_cast<const Nif::NiParticleGrowFade*>(e.getPtr());
                Ogre::ParticleAffector *affector = partsys->addAffector("GrowFade");
                affector->setParameter("grow_time", Ogre::StringConverter::toString(gf->growTime));
                affector->setParameter("fade_time", Ogre::StringConverter::toString(gf->fadeTime));
            }
            else if(e->recType == Nif::RC_NiParticleColorModifier)
            {
                // TODO: Implement (Ogre::ColourInterpolatorAffector?)
            }
            else if(e->recType == Nif::RC_NiGravity)
            {
                // TODO: Implement (Ogre::LinearForceAffector?)
            }
            else
                warn("Unhandled particle modifier "+e->recName);
            e = e->extra;
        }
    }

    Ogre::ParticleSystem *createParticleSystem(Ogre::SceneManager *sceneMgr, Ogre::Entity *entitybase,
                                               const Nif::Node *partnode)
    {
        const Nif::NiAutoNormalParticlesData *particledata = NULL;
        if(partnode->recType == Nif::RC_NiAutoNormalParticles)
            particledata = static_cast<const Nif::NiAutoNormalParticles*>(partnode)->data.getPtr();
        else if(partnode->recType == Nif::RC_NiRotatingParticles)
            particledata = static_cast<const Nif::NiRotatingParticles*>(partnode)->data.getPtr();

        Ogre::ParticleSystem *partsys = sceneMgr->createParticleSystem();
        try {
            std::string fullname = mName+"@index="+Ogre::StringConverter::toString(partnode->recIndex);
            if(partnode->name.length() > 0)
                fullname += "@type="+partnode->name;
            Misc::StringUtils::toLower(fullname);

            const Nif::NiTexturingProperty *texprop = NULL;
            const Nif::NiMaterialProperty *matprop = NULL;
            const Nif::NiAlphaProperty *alphaprop = NULL;
            const Nif::NiVertexColorProperty *vertprop = NULL;
            const Nif::NiZBufferProperty *zprop = NULL;
            const Nif::NiSpecularProperty *specprop = NULL;
            bool needTangents = false;

            getNodeProperties(partnode, texprop, matprop, alphaprop, vertprop, zprop, specprop);
            partsys->setMaterialName(NIFMaterialLoader::getMaterial(particledata, fullname, mGroup,
                                                                    texprop, matprop, alphaprop,
                                                                    vertprop, zprop, specprop,
                                                                    needTangents));

            partsys->setDefaultDimensions(particledata->particleSize, particledata->particleSize);
            partsys->setCullIndividually(false);
            partsys->setParticleQuota(particledata->numParticles);

            Nif::ControllerPtr ctrl = partnode->controller;
            while(!ctrl.empty())
            {
                if(ctrl->recType == Nif::RC_NiParticleSystemController)
                {
                    const Nif::NiParticleSystemController *partctrl = static_cast<const Nif::NiParticleSystemController*>(ctrl.getPtr());

                    createParticleEmitterAffectors(partsys, partctrl);
                    if(!partctrl->emitter.empty() && !partsys->isAttached())
                    {
                        int trgtid = NIFSkeletonLoader::lookupOgreBoneHandle(mName, partctrl->emitter->recIndex);
                        Ogre::Bone *trgtbone = entitybase->getSkeleton()->getBone(trgtid);
                        entitybase->attachObjectToBone(trgtbone->getName(), partsys);
                    }
                }
                ctrl = ctrl->next;
            }

            if(!partsys->isAttached())
                entitybase->attachObjectToBone(partnode->name, partsys);
        }
        catch(std::exception &e) {
            std::cerr<< "Particles exception: "<<e.what() <<std::endl;
            sceneMgr->destroyParticleSystem(partsys);
            partsys = NULL;
        };
        return partsys;
    }


    NIFMeshLoader(const std::string &name, const std::string &group)
      : mName(name), mGroup(group), mShapeIndex(~(size_t)0)
    { }

    virtual void loadResource(Ogre::Resource *resource)
    {
        Ogre::Mesh *mesh = dynamic_cast<Ogre::Mesh*>(resource);
        OgreAssert(mesh, "Attempting to load a mesh into a non-mesh resource!");

        Nif::NIFFile::ptr nif = Nif::NIFFile::create(mName);
        if(mShapeIndex >= nif->numRecords())
        {
            Ogre::SkeletonManager *skelMgr = Ogre::SkeletonManager::getSingletonPtr();
            if(!skelMgr->getByName(mName).isNull())
                mesh->setSkeletonName(mName);
            return;
        }

        const Nif::Node *node = dynamic_cast<const Nif::Node*>(nif->getRecord(0));
        findTriShape(mesh, node);
    }

    void createEntities(Ogre::SceneManager *sceneMgr, const Nif::Node *node, EntityList &entities, int flags=0)
    {
        // Do not create meshes for the collision shape (includes all children)
        if(node->recType == Nif::RC_RootCollisionNode)
            return;

        // Marker objects: just skip the entire node
        /// \todo don't do this in the editor
        if (node->name.find("marker") != std::string::npos)
            return;

        flags |= node->flags;

        Nif::ExtraPtr e = node->extra;
        while(!e.empty())
        {
            if(e->recType == Nif::RC_NiStringExtraData)
            {
                const Nif::NiStringExtraData *sd = static_cast<const Nif::NiStringExtraData*>(e.getPtr());
                // String markers may contain important information
                // affecting the entire subtree of this obj
                if(sd->string == "MRK")
                {
                    // Marker objects. These are only visible in the
                    // editor.
                    return;
                }
            }
            e = e->extra;
        }

        Nif::ControllerPtr ctrl = node->controller;
        while(!ctrl.empty())
        {
            if(ctrl->recType == Nif::RC_NiVisController)
            {
                const Nif::NiVisController *vis = static_cast<const Nif::NiVisController*>(ctrl.getPtr());

                int trgtid = NIFSkeletonLoader::lookupOgreBoneHandle(mName, ctrl->target->recIndex);
                Ogre::Bone *trgtbone = entities.mSkelBase->getSkeleton()->getBone(trgtid);
                Ogre::SharedPtr<Ogre::ControllerValue<Ogre::Real> > srcval; /* Filled in later */
                Ogre::SharedPtr<Ogre::ControllerValue<Ogre::Real> > dstval(OGRE_NEW VisController::Value(trgtbone));
                Ogre::SharedPtr<Ogre::ControllerFunction<Ogre::Real> > func(OGRE_NEW VisController::Function(vis->data.getPtr()));

                entities.mControllers.push_back(Ogre::Controller<Ogre::Real>(srcval, dstval, func));
            }
            ctrl = ctrl->next;
        }

        if(node->recType == Nif::RC_NiTriShape)
        {
            const Nif::NiTriShape *shape = static_cast<const Nif::NiTriShape*>(node);

            Ogre::MeshManager &meshMgr = Ogre::MeshManager::getSingleton();
            std::string fullname = mName+"@index="+Ogre::StringConverter::toString(shape->recIndex);
            if(shape->name.length() > 0)
                fullname += "@shape="+shape->name;

            Misc::StringUtils::toLower(fullname);
            Ogre::MeshPtr mesh = meshMgr.getByName(fullname);
            if(mesh.isNull())
            {
                NIFMeshLoader *loader = &sLoaders[fullname];
                *loader = *this;
                loader->mShapeIndex = shape->recIndex;

                mesh = meshMgr.createManual(fullname, mGroup, loader);
                mesh->setAutoBuildEdgeLists(false);
            }

            Ogre::Entity *entity = sceneMgr->createEntity(mesh);
            entity->setVisible(!(flags&0x01));

            entities.mEntities.push_back(entity);
            if(entities.mSkelBase)
            {
                if(entity->hasSkeleton())
                    entity->shareSkeletonInstanceWith(entities.mSkelBase);
                else
                {
                    int trgtid = NIFSkeletonLoader::lookupOgreBoneHandle(mName, shape->recIndex);
                    Ogre::Bone *trgtbone = entities.mSkelBase->getSkeleton()->getBone(trgtid);
                    entities.mSkelBase->attachObjectToBone(trgtbone->getName(), entity);
                }
            }
        }

        if(node->recType == Nif::RC_NiAutoNormalParticles ||
           node->recType == Nif::RC_NiRotatingParticles)
        {
            Ogre::ParticleSystem *partsys = createParticleSystem(sceneMgr, entities.mSkelBase, node);
            if(partsys != NULL)
            {
                partsys->setVisible(!(flags&0x01));
                entities.mParticles.push_back(partsys);
            }
        }

        const Nif::NiNode *ninode = dynamic_cast<const Nif::NiNode*>(node);
        if(ninode)
        {
            const Nif::NodeList &children = ninode->children;
            for(size_t i = 0;i < children.length();i++)
            {
                if(!children[i].empty())
                    createEntities(sceneMgr, children[i].getPtr(), entities, flags);
            }
        }
    }

    void createSkelBase(Ogre::SceneManager *sceneMgr, const Nif::Node *node, EntityList &entities)
    {
        /* This creates an empty mesh to which a skeleton gets attached. This
         * is to ensure we have an entity with a skeleton instance, even if all
         * other meshes are hidden or entities attached to a specific node
         * instead of skinned. */
        std::string fullname = mName;
        Misc::StringUtils::toLower(fullname);

        Ogre::MeshManager &meshMgr = Ogre::MeshManager::getSingleton();
        Ogre::MeshPtr mesh = meshMgr.getByName(fullname);
        if(mesh.isNull())
        {
            NIFMeshLoader *loader = &sLoaders[fullname];
            *loader = *this;

            mesh = meshMgr.createManual(fullname, mGroup, loader);
            mesh->setAutoBuildEdgeLists(false);
        }
        entities.mSkelBase = sceneMgr->createEntity(mesh);
        entities.mEntities.push_back(entities.mSkelBase);
    }

public:
    NIFMeshLoader() : mShapeIndex(~(size_t)0)
    { }

    static void load(Ogre::SceneManager *sceneMgr, EntityList &entities, const std::string &name, const std::string &group)
    {
        Nif::NIFFile::ptr pnif = Nif::NIFFile::create(name);
        Nif::NIFFile &nif = *pnif.get();
        if(nif.numRecords() < 1)
        {
            nif.warn("Found no NIF records in "+name+".");
            return;
        }

        // The first record is assumed to be the root node
        const Nif::Record *r = nif.getRecord(0);
        assert(r != NULL);

        const Nif::Node *node = dynamic_cast<Nif::Node const *>(r);
        if(node == NULL)
        {
            nif.warn("First record in "+name+" was not a node, but a "+
                    r->recName+".");
            return;
        }

        bool hasSkel = Ogre::SkeletonManager::getSingleton().resourceExists(name);
        if(!hasSkel)
            hasSkel = !NIFSkeletonLoader::createSkeleton(name, group, node).isNull();

        NIFMeshLoader meshldr(name, group);
        if(hasSkel)
            meshldr.createSkelBase(sceneMgr, node, entities);
        meshldr.createEntities(sceneMgr, node, entities);
    }
};
NIFMeshLoader::LoaderMap NIFMeshLoader::sLoaders;


EntityList Loader::createEntities(Ogre::SceneNode *parentNode, std::string name, const std::string &group)
{
    EntityList entitylist;

    Misc::StringUtils::toLower(name);
    NIFMeshLoader::load(parentNode->getCreator(), entitylist, name, group);

    for(size_t i = 0;i < entitylist.mEntities.size();i++)
    {
        Ogre::Entity *entity = entitylist.mEntities[i];
        if(!entity->isAttached())
            parentNode->attachObject(entity);
    }

    return entitylist;
}

EntityList Loader::createEntities(Ogre::Entity *parent, const std::string &bonename,
                                  Ogre::SceneNode *parentNode,
                                  std::string name, const std::string &group)
{
    EntityList entitylist;

    Misc::StringUtils::toLower(name);
    NIFMeshLoader::load(parentNode->getCreator(), entitylist, name, group);

    bool isskinned = false;
    for(size_t i = 0;i < entitylist.mEntities.size();i++)
    {
        Ogre::Entity *ent = entitylist.mEntities[i];
        if(entitylist.mSkelBase != ent && ent->hasSkeleton())
        {
            isskinned = true;
            break;
        }
    }

    Ogre::Vector3 scale(1.0f);
    if(bonename.find("Left") != std::string::npos)
        scale.x *= -1.0f;

    if(isskinned)
    {
        std::string filter = "@shape=tri "+bonename;
        Misc::StringUtils::toLower(filter);
        for(size_t i = 0;i < entitylist.mEntities.size();i++)
        {
            Ogre::Entity *entity = entitylist.mEntities[i];
            if(entity->hasSkeleton())
            {
                if(entity == entitylist.mSkelBase ||
                   entity->getMesh()->getName().find(filter) != std::string::npos)
                    parentNode->attachObject(entity);
            }
            else
            {
                if(entity->getMesh()->getName().find(filter) == std::string::npos)
                    entity->detachFromParent();
            }
        }
    }
    else
    {
        for(size_t i = 0;i < entitylist.mEntities.size();i++)
        {
            Ogre::Entity *entity = entitylist.mEntities[i];
            if(!entity->isAttached())
            {
                Ogre::TagPoint *tag = parent->attachObjectToBone(bonename, entity);
                tag->setScale(scale);
            }
        }
    }

    return entitylist;
}


Ogre::SkeletonPtr Loader::getSkeleton(std::string name, const std::string &group)
{
    Ogre::SkeletonPtr skel;

    Misc::StringUtils::toLower(name);
    skel = Ogre::SkeletonManager::getSingleton().getByName(name);
    if(!skel.isNull())
        return skel;

    Nif::NIFFile::ptr nif = Nif::NIFFile::create(name);
    if(nif->numRecords() < 1)
    {
        nif->warn("Found no NIF records in "+name+".");
        return skel;
    }

    // The first record is assumed to be the root node
    const Nif::Record *r = nif->getRecord(0);
    assert(r != NULL);

    const Nif::Node *node = dynamic_cast<const Nif::Node*>(r);
    if(node == NULL)
    {
        nif->warn("First record in "+name+" was not a node, but a "+
                  r->recName+".");
        return skel;
    }

    return NIFSkeletonLoader::createSkeleton(name, group, node);
}


/* More code currently not in use, from the old D source. This was
   used in the first attempt at loading NIF meshes, where each submesh
   in the file was given a separate bone in a skeleton. Unfortunately
   the OGRE skeletons can't hold more than 256 bones, and some NIFs go
   way beyond that. The code might be of use if we implement animated
   submeshes like this (the part of the NIF that is animated is
   usually much less than the entire file, but the method might still
   not be water tight.)

// Insert a raw RGBA image into the texture system.
extern "C" void ogre_insertTexture(char* name, uint32_t width, uint32_t height, void *data)
{
  TexturePtr texture = TextureManager::getSingleton().createManual(
      name,         // name
      "General",    // group
      TEX_TYPE_2D,      // type
      width, height,    // width & height
      0,                // number of mipmaps
      PF_BYTE_RGBA,     // pixel format
      TU_DEFAULT);      // usage; should be TU_DYNAMIC_WRITE_ONLY_DISCARDABLE for
                        // textures updated very often (e.g. each frame)

  // Get the pixel buffer
  HardwarePixelBufferSharedPtr pixelBuffer = texture->getBuffer();

  // Lock the pixel buffer and get a pixel box
  pixelBuffer->lock(HardwareBuffer::HBL_NORMAL); // for best performance use HBL_DISCARD!
  const PixelBox& pixelBox = pixelBuffer->getCurrentLock();

  void *dest = pixelBox.data;

  // Copy the data
  memcpy(dest, data, width*height*4);

  // Unlock the pixel buffer
  pixelBuffer->unlock();
}


*/

} // nsmaepace NifOgre
