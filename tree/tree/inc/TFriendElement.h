// @(#)root/tree:$Id$
// Author: Rene Brun   07/04/2001

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/
#ifndef ROOT_TFriendElement
#define ROOT_TFriendElement


//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TFriendElement                                                       //
//                                                                      //
// A TFriendElement TF describes a TTree object TF in a file.           //
// When a TFriendElement TF is added to the list of friends of an   //
// existing TTree T, any variable from TF can be referenced in a query  //
// to T.                                                                //
//                                                                      //
//////////////////////////////////////////////////////////////////////////


#include "TNamed.h"

class TFile;
class TTree;
class TClass;

class TFriendElement : public TNamed {

protected:
   TTree        *fParentTree;  ///<! pointer to the parent TTree
   TTree        *fTree;        ///<! pointer to the TTree described by this element
   TFile        *fFile;        ///<! pointer to the file containing the friend TTree
   TString       fTreeName;    ///<  name of the friend TTree
   bool          fOwnFile;     ///<  true if file is managed by this class

   TFriendElement(const TFriendElement&) = delete;
   TFriendElement& operator=(const TFriendElement&) = delete;

   friend void TFriendElement__SetTree(TTree *tree, TList *frlist);

public:
   enum EStatusBits {
      kFromChain = BIT(9),  // Indicates a TChain inserted this element in one of its content TTree
      kUpdated   = BIT(10),  // Indicates that the chain 'fTree' went through a LoadTree
      kUpdatedForChain = BIT(11), // Same as kUpdated, but detected only by the chain itself and not the current tree
   };
   TFriendElement();
   TFriendElement(TTree *tree, const char *treename, const char *filename);
   TFriendElement(TTree *tree, const char *treename, TFile *file);
   TFriendElement(TTree *tree, TTree* friendtree, const char *alias);
   ~TFriendElement() override;
   virtual TTree      *Connect();
   virtual TTree      *DisConnect();
   virtual TFile      *GetFile();
   virtual TTree      *GetParentTree() const {return fParentTree;}
   virtual TTree      *GetTree();
   /// Get the actual TTree name of the friend.
   /// If an alias is present, it can be retrieved with GetName().
   virtual const char *GetTreeName() const {return fTreeName.Data();}
   void        ls(Option_t *option="") const override;
           void        Reset() { fTree = nullptr; fFile = nullptr; }
           bool        IsUpdated() const { return TestBit(kUpdated); }
           bool        IsUpdatedForChain() const { return TestBit(kUpdatedForChain); }
           void        ResetUpdated() { ResetBit(kUpdated); }
           void        ResetUpdatedForChain() { ResetBit(kUpdatedForChain); }
           void        MarkUpdated() { SetBit(kUpdated); SetBit(kUpdatedForChain); }
   void        RecursiveRemove(TObject *obj) override;


   ClassDefOverride(TFriendElement,2)  //A friend element of another TTree
};

#endif

