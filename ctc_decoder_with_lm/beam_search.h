/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2018 Mozilla Corporation
 *
 * The code was taken from Mozilla DeepSpeech project:
 * https://github.com/mozilla/DeepSpeech/tree/master/native_client
 *
 */

#ifndef BEAM_SEARCH_H
#define BEAM_SEARCH_H

#include "alphabet.h"
#include "trie_node.h"
#include "tensorflow/core/util/ctc/ctc_beam_search.h"
#include "external_lm.h"
#include "kenlm/lm/model.hh"
#include <iostream>

typedef lm::ngram::QuantArrayTrieModel Model;

struct WordLMBeamState {
  float language_model_score;
  float score;
  std::string incomplete_word;
  TrieNode *incomplete_word_trie_node;
  std::vector<std::string> prefix;
  Model::State model_state;
  bool new_word;
};

class WordLMBeamScorer : public tensorflow::ctc::BaseBeamScorer<WordLMBeamState> {
  public:
    WordLMBeamScorer(const std::string &kenlm_path, const std::string &trie_path, 
                     const std::string &alphabet_path,
                     float alpha, float beta, float trie_weight, int mode, 
                     const std::string &neural_lm_path)
      : model_(kenlm_path.c_str(), GetLMConfig())
      , alphabet_(alphabet_path.c_str())
      , alpha_(alpha)
      , beta_(beta)
      , trie_weight_(trie_weight)
      , mode_(mode)      
    {
      Model::State out;
      std::ifstream in(trie_path, std::ios::in);
      TrieNode::ReadFromStream(in, trieRoot_, alphabet_.GetSize());
      oov_score_ = model_.FullScore(model_.NullContextState(), model_.GetVocabulary().NotFound(), out).prob;     
      if (mode_ == 1) {
        neural_lm_ = ExternalLanguageModel(neural_lm_path.c_str());
        std::cout<<"============>>>>>>>>>>>>> TESTING NEURAL LM"<<std::endl;
        std::vector<std::string> ex2;
        ex2.push_back("where");  
        ex2.push_back("who");
        ex2.push_back("go");
        ex2.push_back("table");
        ex2.push_back("sit");  
        std::cout<<"String: "<<ex2<<"   Score: "<<neural_lm_.score_phrase(ex2)<<std::endl;

        std::vector<std::string> ex3;
        ex3.push_back("the");  
        ex3.push_back("fox");
        ex3.push_back("jumps");
        ex3.push_back("on");
        ex3.push_back("the");  
        ex3.push_back("box");  
        std::cout<<"String: "<<ex3<<"   Score: "<<neural_lm_.score_phrase(ex3)<<std::endl;
        

        std::vector<std::string> ex4;
        ex4.push_back("the");  
        ex4.push_back("fox");
        ex4.push_back("jump");
        ex4.push_back("at");
        ex4.push_back("the");  
        ex4.push_back("box");  
        std::cout<<"String: "<<ex4<<"   Score: "<<neural_lm_.score_phrase(ex4)<<std::endl;
        std::cout<<"============>>>>>>>>>>>>>END TESTING NEURAL LM"<<std::endl;
      }
    }

    virtual ~WordLMBeamScorer() {
      delete trieRoot_;
    }

    // State initialization.
    void InitializeState(WordLMBeamState* root) const {
      root->language_model_score = 0.f;
      root->score = 0.f;
      root->incomplete_word.clear();
      root->incomplete_word_trie_node = trieRoot_;
      root->prefix.clear();
      root->model_state = model_.BeginSentenceState();
      root->new_word = false;
    }
    // ExpandState is called when expanding a beam to one of its children.
    // Called at most once per child beam. In the simplest case, no state
    // expansion is done.
    void ExpandState(const WordLMBeamState& from_state, int from_label,
                     WordLMBeamState* to_state, int to_label) const {
      CopyState(from_state, to_state);

      if (!alphabet_.IsSpace(to_label)) {
        to_state->incomplete_word += alphabet_.StringFromLabel(to_label);
        
        float min_unigram_score = -100.f;
        TrieNode *trie_node = from_state.incomplete_word_trie_node;
        if (trie_node != nullptr) {
          trie_node = trie_node->GetChildAt(to_label);
          to_state->incomplete_word_trie_node = trie_node;

          if (trie_node != nullptr) {
            min_unigram_score = trie_node->GetMinUnigramScore();
          }
        }
        to_state->score = min_unigram_score;
      } else {
        if (from_label == to_label) return;
        float lm_score;

        to_state->prefix.push_back(to_state->incomplete_word);
        to_state->incomplete_word.clear();
        to_state->incomplete_word_trie_node = trieRoot_;
        to_state->new_word = true;

        lm_score = ScoreNGram(to_state->prefix);
 
        to_state->language_model_score = lm_score;

      }
    }
    // ExpandStateEnd is called after decoding has finished. Its purpose is to
    // allow a final scoring of the beam in its current state, before resorting
    // and retrieving the TopN requested candidates. Called at most once per beam.
    void ExpandStateEnd(WordLMBeamState* state) const {
    
      float lm_score;
      if (state->incomplete_word.size() > 0) {
        state->prefix.push_back(state->incomplete_word);
        state->incomplete_word.clear();
        state->incomplete_word_trie_node = trieRoot_;
        state->new_word = true;
        lm_score = ScoreNGram(state->prefix);
        state->language_model_score = lm_score;
      }

    }
    // GetStateExpansionScore should be an inexpensive method to retrieve the
    // (cached) expansion score computed within ExpandState. The score is
    // multiplied (log-addition) with the input score at the current step from
    // the network.
    //
    // The score returned should be a log-probability. In the simplest case, as
    // there's no state expansion logic, the expansion score is zero.
    float GetStateExpansionScore(const WordLMBeamState& state,
                                 float previous_score) const {
      float score = previous_score;
      if (state.new_word)
        score += alpha_ * state.language_model_score + beta_;
      else
        score += trie_weight_ * state.score;
      return score;
    }
    // GetStateEndExpansionScore should be an inexpensive method to retrieve the
    // (cached) expansion score computed within ExpandStateEnd. The score is
    // multiplied (log-addition) with the final probability of the beam.
    //
    // The score returned should be a log-probability.
    float GetStateEndExpansionScore(const WordLMBeamState& state) const {
      float score = 0;
      if (state.new_word)
        score += alpha_ * state.language_model_score + beta_; 

      return score;
    }

    void SetAlpha(float alpha) {
      this->alpha_ = alpha;
    }

    void SetBeta(float beta) {
      this->beta_ = beta;
    }

    void SetTrieWeight (float trie_weight) {
      this->trie_weight_ = trie_weight;
    }


  private:
    ExternalLanguageModel neural_lm_;
    Model model_;
    Alphabet alphabet_;
    float alpha_;
    float beta_;
    float trie_weight_;
    TrieNode *trieRoot_;
    float oov_score_;
    int mode_;

    lm::ngram::Config GetLMConfig() {
      lm::ngram::Config config;
      config.load_method = util::POPULATE_OR_READ;
      return config;
    }


    bool IsOOV(const std::string& word) const {
      auto &vocabulary = model_.GetVocabulary();
      return vocabulary.Index(word) == vocabulary.NotFound();
    }

    float ScoreNGram(const std::vector<std::string>& prefix) const {
      float prob;
      if (mode_ == 0) {             
        Model::State state, out_state;
        model_.NullContextWrite(&state);
        int order = model_.Order();
        int ngram_start = 0;
        int ngram_end = prefix.size();
        if (prefix.size() < order) {
          for (size_t i = 0; i < order - prefix.size(); ++i) {
            lm::WordIndex word_index = model_.BaseVocabulary().Index("<s>");
            prob = model_.BaseScore(&state, word_index, &out_state);
            state = out_state;
          }
        } else {
          ngram_start = ngram_end - order;
        }
        for (size_t i = ngram_start; i < ngram_end; ++i) {
          lm::WordIndex word_index = model_.BaseVocabulary().Index(prefix[i]);
          if (word_index == model_.BaseVocabulary().NotFound()) {
            return -100.;
          }
          
          prob = model_.BaseScore(&state, word_index, &out_state);
          state = out_state;
        }
      
      } else {      
        prob = neural_lm_.score_phrase(prefix);        
      }
      //std::cout<<"Mode: "<<mode_<<"    "<<prob<<std::endl;
      return prob;      
    }


    void CopyState(const WordLMBeamState& from, WordLMBeamState* to) const {
      to->language_model_score = 0;
      to->score = 0.f;
      to->incomplete_word = from.incomplete_word;
      to->incomplete_word_trie_node = from.incomplete_word_trie_node;
      to->prefix = from.prefix;
      to->model_state = from.model_state;
      to->new_word = false;
    }
};

#endif /* BEAM_SEARCH_H */
