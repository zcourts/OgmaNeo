// ----------------------------------------------------------------------------
//  OgmaNeo
//  Copyright(c) 2016 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of OgmaNeo is licensed to you under the terms described
//  in the OGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#include "Architect.h"

#include "Agent.h"
#include "Hierarchy.h"

// Encoders
#include "SparseFeaturesChunk.h"
#include "SparseFeaturesDelay.h"
#include "SparseFeaturesSTDP.h"

#include <iostream>

using namespace ogmaneo;

const std::string ogmaneo::ParameterModifier::_boolTrue = "true";
const std::string ogmaneo::ParameterModifier::_boolFalse = "false";

void Architect::initialize(unsigned int seed, const std::shared_ptr<Resources> &resources) {
    _rng.seed(seed);

    _resources = resources;
}

ParameterModifier Architect::addInputLayer(const Vec2i &size) {
    InputLayer inputLayer;
    inputLayer._size = size;

    _inputLayers.push_back(inputLayer);

    ParameterModifier pm;

    pm._target = &_inputLayers.back()._params;

    return pm;
}

ParameterModifier Architect::addActionLayer(const Vec2i &size, const Vec2i &tileSize) {
    ActionLayer actionLayer;
    actionLayer._size = size;
    actionLayer._tileSize = tileSize;

    _actionLayers.push_back(actionLayer);

    ParameterModifier pm;

    pm._target = &_actionLayers.back()._params;

    return pm;
}

ParameterModifier Architect::addHigherLayer(const Vec2i &size, SparseFeaturesType type) {
    HigherLayer higherLayer;
    higherLayer._size = size;
    higherLayer._type = type;

    _higherLayers.push_back(higherLayer);

    ParameterModifier pm;

    pm._target = &_higherLayers.back()._params;

    return pm;
}

std::shared_ptr<class Hierarchy> Architect::generateHierarchy(std::unordered_map<std::string, std::string> &additionalParams) {
    std::shared_ptr<Hierarchy> h = std::make_shared<Hierarchy>();

    h->_rng = _rng;
    h->_resources = _resources;

    h->_inputImages.resize(_inputLayers.size());

    std::vector<bool> shouldPredict(_inputLayers.size());

    for (int i = 0; i < _inputLayers.size(); i++) {
        h->_inputImages[i] = cl::Image2D(_resources->_cs->getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), _inputLayers[i]._size.x, _inputLayers[i]._size.y);

        /*if (_inputLayers[i]._params.find("in_predict") != _inputLayers[i]._params.end()) {
        if (_inputLayers[i]._params["in_predict"] == ParameterModifier::_boolTrue) {
        h->_predictions.push_back(ValueField2D(_inputLayers[i]._size));

        shouldPredict[i] = true;
        }
        else
        shouldPredict[i] = false;
        }
        else {*/
        h->_predictions.push_back(ValueField2D(_inputLayers[i]._size));

        shouldPredict[i] = true;
        //}
    }

    std::shared_ptr<ComputeProgram> hProg;

    if (_resources->_programs.find("hierarchy") == _resources->_programs.end()) {
        hProg = std::make_shared<ComputeProgram>();
        hProg->loadHierarchyKernel(*_resources->_cs);
    }
    else
        hProg = _resources->_programs["hierarchy"];

    std::shared_ptr<ComputeProgram> pProg;

    if (_resources->_programs.find("predictor") == _resources->_programs.end()) {
        pProg = std::make_shared<ComputeProgram>();
        pProg->loadPredictorKernel(*_resources->_cs);
    }
    else
        pProg = _resources->_programs["predictor"];

    std::vector<Predictor::PredLayerDesc> pLayerDescs(_higherLayers.size());
    std::vector<FeatureHierarchy::LayerDesc> hLayerDescs(_higherLayers.size());

    cl_float2 initWeightRange = { -0.01f, 0.01f };

    if (additionalParams.find("ad_initWeightRange") != additionalParams.end()) {
        Vec2f range = ParameterModifier::parseVec2f(additionalParams["ad_initWeightRange"]);

        initWeightRange = { range.x, range.y };
    }

    // Fill out layer descs
    for (int l = 0; l < _higherLayers.size(); l++) {
        if (_higherLayers[l]._params.find("hl_poolSteps") != _higherLayers[l]._params.end())
            hLayerDescs[l]._poolSteps = std::stoi(_higherLayers[l]._params["hl_poolSteps"]);

        hLayerDescs[l]._sfDesc = sfDescFromName(l, _higherLayers[l]._type, _higherLayers[l]._size, SparseFeatures::_feedForwardRecurrent, _higherLayers[l]._params);

        // P layer desc
        if (_higherLayers[l]._params.find("p_alpha") != _higherLayers[l]._params.end())
            pLayerDescs[l]._alpha = std::stof(_higherLayers[l]._params["p_alpha"]);

        if (_higherLayers[l]._params.find("p_radius") != _higherLayers[l]._params.end())
            pLayerDescs[l]._radius = std::stoi(_higherLayers[l]._params["p_radius"]);
    }

    h->_p.createRandom(*_resources->_cs, *hProg, *pProg, pLayerDescs, hLayerDescs, initWeightRange, _rng);

    // Create readout layers
    h->_readoutLayers.resize(h->_predictions.size());

    for (int i = 0; i < h->_readoutLayers.size(); i++) {
        std::vector<PredictorLayer::VisibleLayerDesc> vlds(1);

        vlds.front()._size = { _higherLayers.front()._size.x, _higherLayers.front()._size.y };

        if (_inputLayers[i]._params.find("in_p_alpha") != _inputLayers[i]._params.end())
            vlds.front()._alpha = std::stof(_inputLayers[i]._params["in_p_alpha"]);

        if (_inputLayers[i]._params.find("in_p_radius") != _inputLayers[i]._params.end())
            vlds.front()._radius = std::stoi(_inputLayers[i]._params["in_p_radius"]);

        h->_readoutLayers[i].createRandom(*_resources->_cs, *pProg, cl_int2{ h->_predictions[i].getSize().x, h->_predictions[i].getSize().y }, vlds, nullptr, initWeightRange, _rng);
    }

    return h;
}

std::shared_ptr<class Agent> Architect::generateAgent(std::unordered_map<std::string, std::string> &additionalParams) {
    std::shared_ptr<Agent> a = std::make_shared<Agent>();

    a->_rng = _rng;
    a->_resources = _resources;

    a->_inputImages.resize(_inputLayers.size());

    for (int i = 0; i < _inputLayers.size(); i++)
        a->_inputImages[i] = cl::Image2D(_resources->_cs->getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), _inputLayers[i]._size.x, _inputLayers[i]._size.y);

    std::vector<cl_int2> actionSizes(_actionLayers.size());
    std::vector<cl_int2> actionTileSizes(_actionLayers.size());

    for (int i = 0; i < _actionLayers.size(); i++) {
        a->_actions.push_back(ValueField2D(_actionLayers[i]._size));

        actionSizes[i] = { _actionLayers[i]._size.x, _actionLayers[i]._size.y };
        actionTileSizes[i] = { _actionLayers[i]._tileSize.x, _actionLayers[i]._tileSize.y };
    }

    std::shared_ptr<ComputeProgram> asProg;

    if (_resources->_programs.find("agentSwarm") == _resources->_programs.end()) {
        asProg = std::make_shared<ComputeProgram>();
        asProg->loadAgentSwarmKernel(*_resources->_cs);
    }
    else
        asProg = _resources->_programs["agentSwarm"];

    std::shared_ptr<ComputeProgram> hProg;

    if (_resources->_programs.find("hierarchy") == _resources->_programs.end()) {
        hProg = std::make_shared<ComputeProgram>();
        hProg->loadHierarchyKernel(*_resources->_cs);
    }
    else
        hProg = _resources->_programs["hierarchy"];

    std::shared_ptr<ComputeProgram> pProg;

    if (_resources->_programs.find("predictor") == _resources->_programs.end()) {
        pProg = std::make_shared<ComputeProgram>();
        pProg->loadPredictorKernel(*_resources->_cs);
    }
    else
        pProg = _resources->_programs["predictor"];

    std::vector<std::vector<AgentSwarm::AgentLayerDesc>> aLayerDescs(_higherLayers.size());
    std::vector<Predictor::PredLayerDesc> pLayerDescs(_higherLayers.size());
    std::vector<FeatureHierarchy::LayerDesc> hLayerDescs(_higherLayers.size());

    cl_float2 initWeightRange = { -0.01f, 0.01f };

    if (additionalParams.find("ad_initWeightRange") != additionalParams.end()) {
        Vec2f range = ParameterModifier::parseVec2f(additionalParams["ad_initWeightRange"]);

        initWeightRange = { range.x, range.y };
    }

    // Fill out layer descs
    for (int l = 0; l < _higherLayers.size(); l++) {
        if (_higherLayers[l]._params.find("hl_poolSteps") != _higherLayers[l]._params.end())
            hLayerDescs[l]._poolSteps = std::stoi(_higherLayers[l]._params["hl_poolSteps"]);

        hLayerDescs[l]._sfDesc = sfDescFromName(l, _higherLayers[l]._type, _higherLayers[l]._size, SparseFeatures::_feedForwardRecurrent, _higherLayers[l]._params);

        // P layer desc
        if (_higherLayers[l]._params.find("p_alpha") != _higherLayers[l]._params.end())
            pLayerDescs[l]._alpha = std::stof(_higherLayers[l]._params["p_alpha"]);

        if (_higherLayers[l]._params.find("p_radius") != _higherLayers[l]._params.end())
            pLayerDescs[l]._radius = std::stoi(_higherLayers[l]._params["p_radius"]);

        // A layer desc
        if (l == _higherLayers.size() - 1) {
            aLayerDescs[l].resize(_actionLayers.size());

            for (int i = 0; i < _actionLayers.size(); i++) {
                if (_actionLayers[i]._params.find("a_radius") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._radius = std::stoi(_actionLayers[i]._params["a_radius"]);

                if (_actionLayers[i]._params.find("a_qAlpha") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._qAlpha = std::stof(_actionLayers[i]._params["a_qAlpha"]);

                if (_actionLayers[i]._params.find("a_actionAlpha") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._actionAlpha = std::stof(_actionLayers[i]._params["a_actionAlpha"]);

                if (_actionLayers[i]._params.find("a_qGamma") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._qGamma = std::stof(_actionLayers[i]._params["a_qGamma"]);

                if (_actionLayers[i]._params.find("a_qLambda") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._qLambda = std::stof(_actionLayers[i]._params["a_qLambda"]);

                if (_actionLayers[i]._params.find("a_actionLambda") != _actionLayers[i]._params.end())
                    aLayerDescs[l][i]._actionLambda = std::stof(_actionLayers[i]._params["a_actionLambda"]);
            }
        }
        else {
            aLayerDescs[l].resize(1);

            if (_higherLayers[l]._params.find("a_radius") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._radius = std::stoi(_higherLayers[l]._params["a_radius"]);

            if (_higherLayers[l]._params.find("a_qAlpha") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._qAlpha = std::stof(_higherLayers[l]._params["a_qAlpha"]);

            if (_higherLayers[l]._params.find("a_actionAlpha") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._actionAlpha = std::stof(_higherLayers[l]._params["a_actionAlpha"]);

            if (_higherLayers[l]._params.find("a_qGamma") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._qGamma = std::stof(_higherLayers[l]._params["a_qGamma"]);

            if (_higherLayers[l]._params.find("a_qLambda") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._qLambda = std::stof(_higherLayers[l]._params["a_qLambda"]);

            if (_higherLayers[l]._params.find("a_actionLambda") != _higherLayers[l]._params.end())
                aLayerDescs[l].front()._actionLambda = std::stof(_higherLayers[l]._params["a_actionLambda"]);
        }
    }

    a->_as.createRandom(*_resources->_cs, *hProg, *pProg, *asProg, actionSizes, actionTileSizes, aLayerDescs, pLayerDescs, hLayerDescs, initWeightRange, _rng);

    return a;
}

std::shared_ptr<SparseFeatures::SparseFeaturesDesc> Architect::sfDescFromName(int layerIndex, SparseFeaturesType type, const Vec2i &size,
    SparseFeatures::InputType inputType, std::unordered_map<std::string, std::string> &params)
{
    std::shared_ptr<SparseFeatures::SparseFeaturesDesc> sfDesc;

    switch (type) {
    case _stdp:
    {
        std::shared_ptr<SparseFeaturesSTDP::SparseFeaturesSTDPDesc> sfDescSTDP = std::make_shared<SparseFeaturesSTDP::SparseFeaturesSTDPDesc>();

        sfDescSTDP->_cs = _resources->_cs;
        sfDescSTDP->_inputType = SparseFeatures::_feedForwardRecurrent;
        sfDescSTDP->_hiddenSize = { size.x, size.y };
        sfDescSTDP->_rng = _rng;

        if (params.find("sfs_inhibitionRadius") != params.end())
            sfDescSTDP->_inhibitionRadius = std::stoi(params["sfs_inhibitionRadius"]);

        if (params.find("sfs_initWeightRange") != params.end()) {
            Vec2f initWeightRange = ParameterModifier::parseVec2f(params["sfs_initWeightRange"]);
            sfDescSTDP->_initWeightRange = { initWeightRange.x, initWeightRange.y };
        }

        if (_resources->_programs.find("stdp") == _resources->_programs.end()) {
            _resources->_programs["stdp"] = sfDescSTDP->_sfcProgram = std::make_shared<ComputeProgram>();

            sfDescSTDP->_sfcProgram->loadSparseFeaturesKernel(*_resources->_cs, _stdp);
        }
        else
            sfDescSTDP->_sfcProgram = _resources->_programs["stdp"];

        if (params.find("sfs_biasAlpha") != params.end())
            sfDescSTDP->_biasAlpha = std::stof(params["sfs_biasAlpha"]);

        if (params.find("sfs_activeRatio") != params.end())
            sfDescSTDP->_activeRatio = std::stof(params["sfs_activeRatio"]);

        if (params.find("sfs_gamma") != params.end())
            sfDescSTDP->_gamma = std::stof(params["sfs_gamma"]);

        if (layerIndex == 0) {
            sfDescSTDP->_visibleLayerDescs.resize(_inputLayers.size() + 1);

            for (int i = 0; i < _inputLayers.size(); i++) {
                sfDescSTDP->_visibleLayerDescs[i]._ignoreMiddle = false;

                sfDescSTDP->_visibleLayerDescs[i]._size = { _inputLayers[i]._size.x, _inputLayers[i]._size.y };

                if (_inputLayers[i]._params.find("sfs_ff_radius") != _inputLayers[i]._params.end())
                    sfDescSTDP->_visibleLayerDescs[i]._radius = std::stoi(_inputLayers[i]._params["sfs_ff_radius"]);

                if (_inputLayers[i]._params.find("sfs_ff_weightAlpha") != _inputLayers[i]._params.end())
                    sfDescSTDP->_visibleLayerDescs[i]._weightAlpha = std::stof(_inputLayers[i]._params["sfs_ff_weightAlpha"]);

                if (_inputLayers[i]._params.find("sfs_ff_lambda") != _inputLayers[i]._params.end())
                    sfDescSTDP->_visibleLayerDescs[i]._lambda = std::stof(_inputLayers[i]._params["sfs_ff_lambda"]);
            }

            // Recurrent
            {
                sfDescSTDP->_visibleLayerDescs.back()._ignoreMiddle = true;

                sfDescSTDP->_visibleLayerDescs.back()._size = { _higherLayers[layerIndex]._size.x, _higherLayers[layerIndex]._size.y };

                if (params.find("sfs_r_radius") != params.end())
                    sfDescSTDP->_visibleLayerDescs.back()._radius = std::stoi(params["sfs_r_radius"]);

                if (params.find("sfs_r_weightAlpha") != params.end())
                    sfDescSTDP->_visibleLayerDescs.back()._weightAlpha = std::stof(params["sfs_r_weightAlpha"]);

                if (params.find("sfs_r_lambda") != params.end())
                    sfDescSTDP->_visibleLayerDescs.back()._lambda = std::stof(params["sfs_r_lambda"]);
            }
        }
        else {
            sfDescSTDP->_visibleLayerDescs.resize(2);

            // Feed forward
            {
                sfDescSTDP->_visibleLayerDescs[0]._ignoreMiddle = false;

                sfDescSTDP->_visibleLayerDescs[0]._size = { _higherLayers[layerIndex - 1]._size.x, _higherLayers[layerIndex - 1]._size.y };

                if (params.find("sfs_ff_radius") != params.end())
                    sfDescSTDP->_visibleLayerDescs[0]._radius = std::stoi(params["sfs_ff_radius"]);

                if (params.find("sfs_ff_weightAlpha") != params.end())
                    sfDescSTDP->_visibleLayerDescs[0]._weightAlpha = std::stof(params["sfs_ff_weightAlpha"]);

                if (params.find("sfs_ff_lambda") != params.end())
                    sfDescSTDP->_visibleLayerDescs[0]._lambda = std::stof(params["sfs_ff_lambda"]);
            }

            // Recurrent
            {
                sfDescSTDP->_visibleLayerDescs[1]._ignoreMiddle = true;

                sfDescSTDP->_visibleLayerDescs[1]._size = { _higherLayers[layerIndex]._size.x, _higherLayers[layerIndex]._size.y };

                if (params.find("sfs_r_radius") != params.end())
                    sfDescSTDP->_visibleLayerDescs[1]._radius = std::stoi(params["sfs_r_radius"]);

                if (params.find("sfs_r_weightAlpha") != params.end())
                    sfDescSTDP->_visibleLayerDescs[1]._weightAlpha = std::stof(params["sfs_r_weightAlpha"]);

                if (params.find("sfs_r_lambda") != params.end())
                    sfDescSTDP->_visibleLayerDescs[1]._lambda = std::stof(params["sfs_r_lambda"]);
            }
        }

        sfDesc = sfDescSTDP;

        break;
    }
    case _delay:
    {
        std::shared_ptr<SparseFeaturesDelay::SparseFeaturesDelayDesc> sfDescDelay = std::make_shared<SparseFeaturesDelay::SparseFeaturesDelayDesc>();

        sfDescDelay->_cs = _resources->_cs;
        sfDescDelay->_inputType = SparseFeatures::_feedForward;
        sfDescDelay->_hiddenSize = { size.x, size.y };
        sfDescDelay->_rng = _rng;

        if (params.find("sfd_inhibitionRadius") != params.end())
            sfDescDelay->_inhibitionRadius = std::stoi(params["sfd_inhibitionRadius"]);

        if (params.find("sfd_initWeightRange") != params.end()) {
            Vec2f initWeightRange = ParameterModifier::parseVec2f(params["sfd_initWeightRange"]);
            sfDescDelay->_initWeightRange = { initWeightRange.x, initWeightRange.y };
        }

        if (_resources->_programs.find("delay") == _resources->_programs.end()) {
            _resources->_programs["delay"] = sfDescDelay->_sfcProgram = std::make_shared<ComputeProgram>();

            sfDescDelay->_sfcProgram->loadSparseFeaturesKernel(*_resources->_cs, _delay);
        }
        else
            sfDescDelay->_sfcProgram = _resources->_programs["delay"];

        if (params.find("sfd_biasAlpha") != params.end())
            sfDescDelay->_biasAlpha = std::stof(params["sfd_biasAlpha"]);

        if (params.find("sfd_activeRatio") != params.end())
            sfDescDelay->_activeRatio = std::stof(params["sfd_activeRatio"]);

        if (layerIndex == 0) {
            sfDescDelay->_visibleLayerDescs.resize(_inputLayers.size());

            for (int i = 0; i < _inputLayers.size(); i++) {
                sfDescDelay->_visibleLayerDescs[i]._ignoreMiddle = false;

                sfDescDelay->_visibleLayerDescs[i]._size = { _inputLayers[i]._size.x, _inputLayers[i]._size.y };

                if (_inputLayers[i]._params.find("sfd_ff_radius") != _inputLayers[i]._params.end())
                    sfDescDelay->_visibleLayerDescs[i]._radius = std::stoi(_inputLayers[i]._params["sfd_ff_radius"]);

                if (_inputLayers[i]._params.find("sfd_ff_weightAlpha") != _inputLayers[i]._params.end())
                    sfDescDelay->_visibleLayerDescs[i]._weightAlpha = std::stof(_inputLayers[i]._params["sfd_ff_weightAlpha"]);

                if (_inputLayers[i]._params.find("sfd_ff_lambda") != _inputLayers[i]._params.end())
                    sfDescDelay->_visibleLayerDescs[i]._lambda = std::stof(_inputLayers[i]._params["sfd_ff_lambda"]);

                if (_inputLayers[i]._params.find("sfd_ff_gamma") != _inputLayers[i]._params.end())
                    sfDescDelay->_visibleLayerDescs[i]._lambda = std::stof(_inputLayers[i]._params["sfd_ff_gamma"]);
            }
        }
        else {
            sfDescDelay->_visibleLayerDescs.resize(1);

            // Feed forward
            {
                sfDescDelay->_visibleLayerDescs[0]._ignoreMiddle = false;

                sfDescDelay->_visibleLayerDescs[0]._size = { _higherLayers[layerIndex - 1]._size.x, _higherLayers[layerIndex - 1]._size.y };

                if (params.find("sfd_ff_radius") != params.end())
                    sfDescDelay->_visibleLayerDescs[0]._radius = std::stoi(params["sfd_ff_radius"]);

                if (params.find("sfd_ff_weightAlpha") != params.end())
                    sfDescDelay->_visibleLayerDescs[0]._weightAlpha = std::stof(params["sfd_ff_weightAlpha"]);

                if (params.find("sfd_ff_lambda") != params.end())
                    sfDescDelay->_visibleLayerDescs[0]._lambda = std::stof(params["sfd_ff_lambda"]);

                if (params.find("sfd_ff_gamma") != params.end())
                    sfDescDelay->_visibleLayerDescs[0]._gamma = std::stof(params["sfd_ff_gamma"]);
            }
        }

        sfDesc = sfDescDelay;

        break;
    }
    case _chunk:
    {
        std::shared_ptr<SparseFeaturesChunk::SparseFeaturesChunkDesc> sfDescChunk = std::make_shared<SparseFeaturesChunk::SparseFeaturesChunkDesc>();

        sfDescChunk->_cs = _resources->_cs;
        sfDescChunk->_inputType = SparseFeatures::_feedForwardRecurrent;
        sfDescChunk->_hiddenSize = { size.x, size.y };
        sfDescChunk->_rng = _rng;

        if (params.find("sfc_chunkSize") != params.end()) {
            Vec2i chunkSize = ParameterModifier::parseVec2i(params["sfc_chunkSize"]);
            sfDescChunk->_chunkSize = { chunkSize.x, chunkSize.y };
        }

        if (params.find("sfc_initWeightRange") != params.end()) {
            Vec2f initWeightRange = ParameterModifier::parseVec2f(params["sfc_initWeightRange"]);
            sfDescChunk->_initWeightRange = { initWeightRange.x, initWeightRange.y };
        }

        if (_resources->_programs.find("chunk") == _resources->_programs.end()) {
            _resources->_programs["chunk"] = sfDescChunk->_sfcProgram = std::make_shared<ComputeProgram>();

            sfDescChunk->_sfcProgram->loadSparseFeaturesKernel(*_resources->_cs, _chunk);
        }
        else
            sfDescChunk->_sfcProgram = _resources->_programs["chunk"];

        if (params.find("sfc_numSamples") != params.end())
            sfDescChunk->_numSamples = std::stoi(params["sfc_numSamples"]);

        if (params.find("sfc_biasAlpha") != params.end())
            sfDescChunk->_biasAlpha = std::stof(params["sfc_biasAlpha"]);

        if (params.find("sfc_gamma") != params.end())
            sfDescChunk->_gamma = std::stof(params["sfc_gamma"]);

        if (layerIndex == 0) {
            sfDescChunk->_visibleLayerDescs.resize(_inputLayers.size() + 1);

            for (int i = 0; i < _inputLayers.size(); i++) {
                sfDescChunk->_visibleLayerDescs[i]._ignoreMiddle = false;

                sfDescChunk->_visibleLayerDescs[i]._size = { _inputLayers[i]._size.x, _inputLayers[i]._size.y };

                if (_inputLayers[i]._params.find("sfc_ff_radius") != _inputLayers[i]._params.end())
                    sfDescChunk->_visibleLayerDescs[i]._radius = std::stoi(_inputLayers[i]._params["sfc_ff_radius"]);

                if (_inputLayers[i]._params.find("sfc_ff_weightAlpha") != _inputLayers[i]._params.end())
                    sfDescChunk->_visibleLayerDescs[i]._weightAlpha = std::stof(_inputLayers[i]._params["sfc_ff_weightAlpha"]);

                if (_inputLayers[i]._params.find("sfc_ff_lambda") != _inputLayers[i]._params.end())
                    sfDescChunk->_visibleLayerDescs[i]._lambda = std::stof(_inputLayers[i]._params["sfc_ff_lambda"]);
            }

            // Recurrent
            {
                sfDescChunk->_visibleLayerDescs.back()._ignoreMiddle = true;

                sfDescChunk->_visibleLayerDescs.back()._size = { _higherLayers[layerIndex]._size.x, _higherLayers[layerIndex]._size.y };

                if (params.find("sfc_r_radius") != params.end())
                    sfDescChunk->_visibleLayerDescs.back()._radius = std::stoi(params["sfc_r_radius"]);

                if (params.find("sfc_r_weightAlpha") != params.end())
                    sfDescChunk->_visibleLayerDescs.back()._weightAlpha = std::stof(params["sfc_r_weightAlpha"]);

                if (params.find("sfc_r_lambda") != params.end())
                    sfDescChunk->_visibleLayerDescs.back()._lambda = std::stof(params["sfc_r_lambda"]);
            }
        }
        else {
            sfDescChunk->_visibleLayerDescs.resize(2);

            // Feed forward
            {
                sfDescChunk->_visibleLayerDescs[0]._ignoreMiddle = false;

                sfDescChunk->_visibleLayerDescs[0]._size = { _higherLayers[layerIndex - 1]._size.x, _higherLayers[layerIndex - 1]._size.y };

                if (params.find("sfc_ff_radius") != params.end())
                    sfDescChunk->_visibleLayerDescs[0]._radius = std::stoi(params["sfc_ff_radius"]);

                if (params.find("sfc_ff_weightAlpha") != params.end())
                    sfDescChunk->_visibleLayerDescs[0]._weightAlpha = std::stof(params["sfc_ff_weightAlpha"]);

                if (params.find("sfc_ff_lambda") != params.end())
                    sfDescChunk->_visibleLayerDescs[0]._lambda = std::stof(params["sfc_ff_lambda"]);
            }

            // Recurrent
            {
                sfDescChunk->_visibleLayerDescs[1]._ignoreMiddle = true;

                sfDescChunk->_visibleLayerDescs[1]._size = { _higherLayers[layerIndex]._size.x, _higherLayers[layerIndex]._size.y };

                if (params.find("sfc_r_radius") != params.end())
                    sfDescChunk->_visibleLayerDescs[1]._radius = std::stoi(params["sfc_r_radius"]);

                if (params.find("sfc_r_weightAlpha") != params.end())
                    sfDescChunk->_visibleLayerDescs[1]._weightAlpha = std::stof(params["sfc_r_weightAlpha"]);

                if (params.find("sfc_r_lambda") != params.end())
                    sfDescChunk->_visibleLayerDescs[1]._lambda = std::stof(params["sfc_r_lambda"]);
            }
        }

        sfDesc = sfDescChunk;

        break;
    }
    }

    return sfDesc;
}

void ValueField2D::load(const schemas::ValueField2D* fbValueField2D, ComputeSystem &cs) {
    _size.x = fbValueField2D->_size()->x();
    _size.y = fbValueField2D->_size()->y();

    flatbuffers::uoffset_t numValues = fbValueField2D->_data()->Length();
    _data.reserve(numValues);
    for (flatbuffers::uoffset_t i = 0; i < numValues; i++)
        _data[i] = fbValueField2D->_data()->Get(i);
}

flatbuffers::Offset<schemas::ValueField2D> ValueField2D::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::Vec2i size(_size.x, _size.y);
    return schemas::CreateValueField2D(builder,
        builder.CreateVector(_data.data(), _data.size()),
        &size);
}

void ParameterModifier::load(const schemas::ParameterModifier* fbParameterModifier, ComputeSystem &cs) {
    flatbuffers::uoffset_t numValues = fbParameterModifier->_target()->Length();
    for (flatbuffers::uoffset_t i = 0; i < numValues; i++)
    {
        const flatbuffers::String* key = fbParameterModifier->_target()->Get(i)->_key();
        (*_target)[key->c_str()] = fbParameterModifier->_target()->Get(i)->_value()->c_str();
    }
}

flatbuffers::Offset<schemas::ParameterModifier> ParameterModifier::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    std::vector<flatbuffers::Offset<schemas::Parameter>> parameters;
    for (auto iterator = _target->begin(); iterator != _target->end(); iterator++) {
        parameters.push_back(schemas::CreateParameter(builder,
            builder.CreateString(iterator->first),      //key
            builder.CreateString(iterator->second)));   //value
    }
    return schemas::CreateParameterModifier(builder,
        builder.CreateVector(parameters));
}

void InputLayer::load(const schemas::InputLayer* fbInputLayer, ComputeSystem &cs) {
    _size = Vec2i(fbInputLayer->_size()->x(), fbInputLayer->_size()->y());

    flatbuffers::uoffset_t numValues = fbInputLayer->_params()->Length();
    for (flatbuffers::uoffset_t i = 0; i < numValues; i++)
    {
        const flatbuffers::String* key = fbInputLayer->_params()->Get(i)->_key();
        _params[key->c_str()] = fbInputLayer->_params()->Get(i)->_value()->c_str();
    }
}

flatbuffers::Offset<schemas::InputLayer> InputLayer::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::Vec2i size(_size.x, _size.y);

    std::vector<flatbuffers::Offset<schemas::Parameter>> parameters;
    for (auto iterator = _params.begin(); iterator != _params.end(); iterator++) {
        parameters.push_back(schemas::CreateParameter(builder,
            builder.CreateString(iterator->first),      //key
            builder.CreateString(iterator->second)));   //value
    }
    return schemas::CreateInputLayer(builder,
        &size, builder.CreateVector(parameters));
}

void ActionLayer::load(const schemas::ActionLayer* fbActionLayer, ComputeSystem &cs) {
    _size = Vec2i(fbActionLayer->_size()->x(), fbActionLayer->_size()->y());
    _tileSize = Vec2i(fbActionLayer->_tileSize()->x(), fbActionLayer->_tileSize()->y());

    flatbuffers::uoffset_t numValues = fbActionLayer->_params()->Length();
    for (flatbuffers::uoffset_t i = 0; i < numValues; i++)
    {
        const flatbuffers::String* key = fbActionLayer->_params()->Get(i)->_key();
        _params[key->c_str()] = fbActionLayer->_params()->Get(i)->_value()->c_str();
    }
}

flatbuffers::Offset<schemas::ActionLayer> ActionLayer::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::Vec2i size(_size.x, _size.y);
    schemas::Vec2i tileSize(_tileSize.x, _tileSize.y);

    std::vector<flatbuffers::Offset<schemas::Parameter>> parameters;
    for (auto iterator = _params.begin(); iterator != _params.end(); iterator++) {
        parameters.push_back(schemas::CreateParameter(builder,
            builder.CreateString(iterator->first),      //key
            builder.CreateString(iterator->second)));   //value
    }
    return schemas::CreateActionLayer(builder,
        &size, &tileSize, builder.CreateVector(parameters));
}

void HigherLayer::load(const schemas::HigherLayer* fbHigherLayer, ComputeSystem &cs) {
    _size = Vec2i(fbHigherLayer->_size()->x(), fbHigherLayer->_size()->y());
    
    switch (fbHigherLayer->_type())
    {
    default:
    case schemas::SparseFeaturesTypeEnum__chunk:    _type = SparseFeaturesType::_chunk; break;
    case schemas::SparseFeaturesTypeEnum__delay:    _type = SparseFeaturesType::_delay; break;
    case schemas::SparseFeaturesTypeEnum__stdp:  _type = SparseFeaturesType::_stdp; break;
    }

    flatbuffers::uoffset_t numValues = fbHigherLayer->_params()->Length();
    for (flatbuffers::uoffset_t i = 0; i < numValues; i++)
    {
        const flatbuffers::String* key = fbHigherLayer->_params()->Get(i)->_key();
        _params[key->c_str()] = fbHigherLayer->_params()->Get(i)->_value()->c_str();
    }
}

flatbuffers::Offset<schemas::HigherLayer> HigherLayer::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    schemas::Vec2i size(_size.x, _size.y);

    schemas::SparseFeaturesTypeEnum type;
    switch (_type)
    {
    default:
    case SparseFeaturesType::_chunk:    type = schemas::SparseFeaturesTypeEnum::SparseFeaturesTypeEnum__chunk; break;
    case SparseFeaturesType::_delay:    type = schemas::SparseFeaturesTypeEnum::SparseFeaturesTypeEnum__delay; break;
    case SparseFeaturesType::_stdp:  type = schemas::SparseFeaturesTypeEnum::SparseFeaturesTypeEnum__stdp; break;
    }

    std::vector<flatbuffers::Offset<schemas::Parameter>> parameters;
    for (auto iterator = _params.begin(); iterator != _params.end(); iterator++) {
        parameters.push_back(schemas::CreateParameter(builder,
            builder.CreateString(iterator->first),      //key
            builder.CreateString(iterator->second)));   //value
    }
    return schemas::CreateHigherLayer(builder,
        &size, type, builder.CreateVector(parameters));
}

void Architect::load(const schemas::Architect* fbArchitect, ComputeSystem &cs) {
    if (!_inputLayers.empty()) {
        assert(_inputLayers.size() == fbArchitect->_inputLayers()->Length());
    }
    else {
        _inputLayers.reserve(fbArchitect->_inputLayers()->Length());
    }
    for (flatbuffers::uoffset_t i = 0; i < fbArchitect->_inputLayers()->Length(); i++) {
        _inputLayers[i].load(fbArchitect->_inputLayers()->Get(i), cs);
    }

    if (!_actionLayers.empty()) {
        assert(_actionLayers.size() == fbArchitect->_actionLayers()->Length());
    }
    else {
        _actionLayers.reserve(fbArchitect->_actionLayers()->Length());
    }
    for (flatbuffers::uoffset_t i = 0; i < fbArchitect->_actionLayers()->Length(); i++) {
        _actionLayers[i].load(fbArchitect->_actionLayers()->Get(i), cs);
    }

    if (!_higherLayers.empty()) {
        assert(_higherLayers.size() == fbArchitect->_higherLayers()->Length());
    }
    else {
        _higherLayers.reserve(fbArchitect->_higherLayers()->Length());
    }
    for (flatbuffers::uoffset_t i = 0; i < fbArchitect->_higherLayers()->Length(); i++) {
        _higherLayers[i].load(fbArchitect->_higherLayers()->Get(i), cs);
    }
}

flatbuffers::Offset<schemas::Architect> Architect::save(flatbuffers::FlatBufferBuilder &builder, ComputeSystem &cs) {
    std::vector<flatbuffers::Offset<schemas::InputLayer>> inputLayers;
    for (InputLayer layer : _inputLayers)
        inputLayers.push_back(layer.save(builder, cs));

    std::vector<flatbuffers::Offset<schemas::ActionLayer>> actionLayers;
    for (ActionLayer layer : _actionLayers)
        actionLayers.push_back(layer.save(builder, cs));

    std::vector<flatbuffers::Offset<schemas::HigherLayer>> higherLayers;
    for (HigherLayer layer : _higherLayers)
        higherLayers.push_back(layer.save(builder, cs));

    return schemas::CreateArchitect(builder,
        builder.CreateVector(inputLayers),
        builder.CreateVector(actionLayers),
        builder.CreateVector(higherLayers));
}

void Architect::load(const std::string &fileName) {
    FILE* file = fopen(fileName.c_str(), "rb");
    fseek(file, 0L, SEEK_END);
    size_t length = ftell(file);
    fseek(file, 0L, SEEK_SET);
    std::vector<uint8_t> data(length);
    fread(data.data(), sizeof(uint8_t), length, file);
    fclose(file);

    flatbuffers::Verifier verifier = flatbuffers::Verifier(data.data(), length);

    bool verified =
        schemas::VerifyArchitectBuffer(verifier) |
        schemas::ArchitectBufferHasIdentifier(data.data());

    if (verified) {
        const schemas::Architect* arch = schemas::GetArchitect(data.data());

        load(arch, *_resources->_cs);
    }

    return; //verified;
}

void Architect::save(const std::string &fileName) {
    flatbuffers::FlatBufferBuilder builder;

    flatbuffers::Offset<schemas::Architect> arch = save(builder, *_resources->_cs);

    // Instruct the builder that this Architect is complete.
    schemas::FinishArchitectBuffer(builder, arch);

    // Get the built buffer and size
    uint8_t *buf = builder.GetBufferPointer();
    size_t size = builder.GetSize();

    flatbuffers::Verifier verifier = flatbuffers::Verifier(buf, size);

    bool verified =
        schemas::VerifyArchitectBuffer(verifier) |
        schemas::ArchitectBufferHasIdentifier(buf);

    if (verified) {
        FILE* file = fopen(fileName.c_str(), "wb");
        fwrite(buf, sizeof(uint8_t), size, file);
        fclose(file);
    }

    return; //verified;
}